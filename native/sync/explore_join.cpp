// Rejoindre une partie : instantané de l'hôte, chargement côté client, nouvelle partie ensemble (D12, D27).
#include "explore_internal.hpp"

namespace e33::explore::detail {

// --- Rejoindre : hôte -------------------------------------------------------------------------
void send_snapshot(uint8_t player)
{
    std::vector<uint8_t> bytes;
    std::string err;
    if (!game::make_snapshot(kHostSnapshotSlot, bytes, err)) {
        bump("snapshot_errors");
        log().write("coop_snapshot_error", JsonObj().add("what", err));
        return;
    }
    coopsave::on_snapshot_sent(bytes);   // hôte d'une copie : la copie suit
    const uint32_t xfer = s.next_xfer++;
    const uint64_t h = fnv64(bytes);
    const std::string level = game::current_level();
    s.net.send_to(player, proto::SaveOffer{xfer, static_cast<uint32_t>(bytes.size()), h, level});
    for (uint32_t off = 0; off < bytes.size(); off += proto::kMaxChunk) {
        proto::SaveChunk c;
        c.xfer = xfer;
        c.offset = off;
        const size_t n = std::min<size_t>(proto::kMaxChunk, bytes.size() - off);
        c.data.assign(bytes.begin() + off, bytes.begin() + off + n);
        s.net.send_to(player, c);
    }
    // Copie dans le dossier du run (fabrication des données de test : jamais versionnée).
    if (!launch_args().run_dir.empty()) {
        std::ofstream f(std::filesystem::path(launch_args().run_dir) / L"snapshot.sav", std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        std::ofstream g(std::filesystem::path(launch_args().run_dir) / (L"snapshot-" + std::to_wstring(xfer) + L".sav"), std::ios::binary);
        g.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    s.peers[player].state = JoinState::Sent;
    s.peers[player].since = s.now;
    bump("snapshots_sent");
    log().write("coop_snapshot_sent", JsonObj().add("player", static_cast<int>(player)).add("xfer", xfer).add("size", static_cast<int64_t>(bytes.size()))
                                         .add("hash", hex64(h)).add("level", level));
}

void host_join_tick()
{
    for (auto& [player, pj] : s.peers) presence::set_joining(player, pj.state == JoinState::Sent, s.now);   // charge notre instantané
    for (auto& [player, pj] : s.peers) {
        // Instantané jamais envoyé pendant la narration du client ni dans les 3 s qui suivent : le 2026-10-23 (essai à
        // deux PC), deux chargements demandés dans la seconde de la fin d'un dialogue sont restés sur l'écran noir.
        auto end = s.remote_narr_end.find(player);
        // Ni pendant sa pause (chargement bloqué menu pause ouvert, 2026-10-24).
        const bool client_calm = !s.remote_narr[player] && !s.remote_pause[player] && (end == s.remote_narr_end.end() || s.now - end->second >= 3.0);
        if (pj.state == JoinState::Joined && pj.resync) {
            // Client entré qui veut une resynchro : il joue avec nous jusqu'à l'envoi (aucun combat joué seul chez lui).
            if (safe_point() && client_calm) { pj.resync = false; send_snapshot(player); }
            continue;
        }
        if (pj.state != JoinState::Pending && pj.state != JoinState::Waiting) continue;
        // Client encore dans l'intro de sa nouvelle partie : on attend sa fin (l'instantané la couperait).
        if (safe_point() && client_calm) { send_snapshot(player); continue; }
        if (!pj.wait_sent) {
            const char* why = game::in_battle() ? "battle" : game::map_change_in_progress() ? "loading" : "narrative";
            s.net.send_to(player, proto::JoinWait{why});
            pj.wait_sent = true;
            pj.state = JoinState::Waiting;
            log().write("coop_join_wait", JsonObj().add("player", static_cast<int>(player)).add("reason", why));
        }
    }
}

// --- Rejoindre : client -----------------------------------------------------------------------

// --- Nouvelle partie ensemble ----------------------------------------------------------------------
// L'hôte lance une nouvelle partie pendant qu'un client attend au menu principal : le client lance la sienne au même
// moment (sans aucune sauvegarde), chacun voit l'intro chez lui, la barrière attend le plus lent, puis l'instantané
// de l'hôte remplace la partie locale du client au premier point sûr (même endroit, même moment de l'histoire).
void start_intro()
{
    s.intro = true;
    s.intro_since = s.now;
    s.intro_sequences_done = 0;
    s.intro_calm_since = -1;
}

void follow_new_game()
{
    if (s.cj != S::ClientJoin::None || !game::main_menu()) {
        bump("new_games_ignored");
        log().write("coop_new_game_ignored", JsonObj().add("main_menu", game::main_menu()).add("join_state", static_cast<int>(s.cj)));
        return;
    }
    std::string err;
    if (!game::start_new_game(true, err)) {
        bump("new_game_errors");
        log().write("coop_error", JsonObj().add("what", "start_new_game: " + err));
        return;
    }
    start_intro();
    coopsave::mark_coop_game();   // après StartNewGame (qui lève la garde) : partie de l'hôte, rien dans les saves perso
    bump("new_games_followed");
    // Pas de bannière ici (jugée inutile par Killian, 2026-10-23) : l'intro qui démarre suffit.
    log().write("coop_new_game_followed", JsonObj());
}

void client_join_tick()
{
    if (s.cj == S::ClientJoin::Receiving && s.now - s.last_chunk_at > 60.0) {
        // Instantané interrompu (aucun morceau depuis 60 s) : on le redemande.
        bump("snapshot_timeouts");
        log().write("coop_snapshot_timeout", JsonObj().add("received", s.inbound ? static_cast<int64_t>(s.inbound->received) : -1));
        s.inbound.reset();
        s.cj = S::ClientJoin::None;
        s.last_chunk_at = s.now;
        s.net.publish(proto::ResyncRequest{"snapshot timeout"}, true);
        return;
    }
    if (s.cj == S::ClientJoin::Loading) {
        // Charger depuis le menu principal ou depuis une partie en cours (même chemin que les saves de test).
        const bool retry = s.cj_load_tries > 0;
        // Un nouvel essai passe outre un « chargement en cours » resté levé (c'est peut-être lui qui bloque).
        if (!game::gi() || (!retry && game::map_change_in_progress())) return;
        if (!game::main_menu() && !game::controller()) return;
        // Pas de chargement pendant un dialogue / une cinématique ni juste après : le 2026-10-23 (essai à deux PC),
        // un chargement demandé dans la même seconde que la fin d'un dialogue est resté sur l'écran noir.
        // Calme de 2 s exigé, 20 s d'attente au plus après l'instantané.
        if (!game::main_menu() && (game::in_dialogue() || game::playing_cinematic() || game::pause_menu_visible())) s.cj_calm_since = -1;
        else if (s.cj_calm_since < 0) s.cj_calm_since = s.now;
        if (s.now - s.cj_since < 20.0 && (s.cj_calm_since < 0 || s.now - s.cj_calm_since < 2.0)) return;
        if (!game::main_menu() && game::pause_menu_visible()) { s.cj_since = s.now; return; }   // pause : on attend sa fermeture, sans limite
        std::string err;
        if (!game::load_save_slot(kCoopSlot, err)) {
            bump("join_errors");
            log().write("coop_join_error", JsonObj().add("what", "load: " + err));
            s.cj = S::ClientJoin::None;
            return;
        }
        ++s.cj_load_tries;
        ++s.cj_loads_total;
        if (retry) bump("join_load_retries");
        s.cj = S::ClientJoin::WaitWorld;
        s.cj_since = s.now;
        s.cj_next_diag = s.now + 10.0;
        s.world_stable = 0;
        log().write("coop_join_loading", JsonObj().add("slot", ue::to_utf8(kCoopSlot)).add("try", s.cj_load_tries));
        return;
    }
    if (s.cj == S::ClientJoin::WaitWorld) {
        if (s.now - s.cj_since < 5) return;   // le chargement ne commence pas tout de suite
        const bool ready = world_ready() && s.cj_loads_total > s.join_stuck_test;
        s.world_stable = ready ? s.world_stable + kPollPeriod : 0;
        if (!ready && s.now >= s.cj_next_diag) {
            // Ce qui manque (journal toutes les 10 s) : aucune attente muette.
            s.cj_next_diag = s.now + 10.0;
            log().write("coop_join_wait_world", JsonObj().add("s", s.now - s.cj_since).add("try", s.cj_load_tries)
                                                    .add("controller", game::controller() != nullptr).add("loading", game::map_change_in_progress())
                                                    .add("pawn", game::explore_pawn() != nullptr).add("battle", game::in_battle())
                                                    .add("dialogue", game::in_dialogue()).add("cinematic", game::playing_cinematic())
                                                    .add("main_menu", game::main_menu()));
        }
        if (!ready && s.now - s.cj_since > 45.0) {
            if (s.cj_load_tries < 2) {
                // Monde jamais prêt 45 s après le chargement : on recharge l'instantané (déjà sur le disque).
                log().write("coop_join_load_stuck", JsonObj().add("try", s.cj_load_tries).add("action", "reload"));
                s.cj = S::ClientJoin::Loading;
                s.cj_since = s.now;
                s.cj_calm_since = -1;
                return;
            }
            // Toujours bloqué : on redemande un instantané à l'hôte (nouvel envoi, nouveaux essais).
            bump("join_load_stuck");
            log().write("coop_join_load_stuck", JsonObj().add("try", s.cj_load_tries).add("action", "resync"));
            s.cj = S::ClientJoin::None;
            s.last_chunk_at = s.now;
            s.net.publish(proto::ResyncRequest{"join load stuck"}, true);
            return;
        }
        if (s.world_stable < 3.0) return;
        s.cj = S::ClientJoin::Joined;
        s.cj_joined_at = s.now;
        s.world_mismatch_streak = 0;
        s.last_event_at = s.now;
        const std::string level = game::current_level();
        s.net.publish(proto::JoinReady{s.joined_xfer, level}, true);
        bump("joins_completed");
        builds::rebaseline();
        ui::clear_status("session");
        log().write("coop_joined", JsonObj().add("xfer", s.joined_xfer).add("level", level).add("wait_s", s.now - s.cj_since));
    }
}

void on_chunk(const proto::SaveChunk& c)
{
    auto& in = s.inbound;
    if (!in || c.xfer != in->xfer || c.offset != in->received || c.offset + c.data.size() > in->size) {
        bump("snapshot_chunk_errors");
        log().write("coop_snapshot_chunk_error", JsonObj().add("xfer", c.xfer).add("offset", c.offset));
        return;
    }
    std::copy(c.data.begin(), c.data.end(), in->data.begin() + c.offset);
    s.last_chunk_at = s.now;
    in->received += static_cast<uint32_t>(c.data.size());
    if (in->received < in->size) return;
    const uint64_t h = fnv64(in->data);
    if (h != in->hash) {
        bump("snapshot_hash_errors");
        log().write("coop_snapshot_hash_error", JsonObj().add("want", hex64(in->hash)).add("got", hex64(h)));
        in.reset();
        return;
    }
    const std::filesystem::path dir = std::filesystem::path(game::saves_dir()) / L"e33coop";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto tmp = dir / L"host.tmp", dst = dir / L"host.sav";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(in->data.data()), static_cast<std::streamsize>(in->data.size()));
    }
    std::filesystem::rename(tmp, dst, ec);
    if (ec) { bump("join_errors"); log().write("coop_join_error", JsonObj().add("what", "write: " + ec.message())); in.reset(); return; }
    bump("snapshots_received");
    log().write("coop_snapshot_received", JsonObj().add("xfer", in->xfer).add("size", static_cast<int64_t>(in->size)).add("hash", hex64(h)).add("level", in->level));
    coopsave::on_host_snapshot(in->data, s.net.name_of ? s.net.name_of(0) : std::string(), in->level);   // copie coop (D15)
    s.joined_xfer = in->xfer;
    s.cj = S::ClientJoin::Loading;
    s.cj_since = s.now;
    s.cj_calm_since = -1;
    s.cj_load_tries = 0;
    in.reset();
}

} // namespace e33::explore::detail
