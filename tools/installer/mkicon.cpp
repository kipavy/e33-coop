// Génère icon.ico (losange doré, comme les losanges de joueur du mod) au build : pas de binaire dans le dépôt.
//   mkicon <sortie.ico>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }

// Couleur d'un point (u, v dans [-1, 1]) : liseré doré, fond sombre, petit losange doré au centre. a = 0 : transparent.
void shade(double u, double v, double& r, double& g, double& b, double& a)
{
    const double d = std::fabs(u) + std::fabs(v);   // distance « losange »
    r = g = b = a = 0;
    if (d > 0.98) return;
    a = 1;
    if (d > 0.80 || d < 0.30) { r = 0xe2; g = 0xcb; b = 0xa0; }   // or (kGold du panneau)
    else { r = 0x14; g = 0x13; b = 0x12; }                          // fond (kPanel)
}

// Image BGRA (de bas en haut) + masque AND, au format ICO (BITMAPINFOHEADER, hauteur doublée).
std::vector<uint8_t> image(int n)
{
    std::vector<uint8_t> px;
    constexpr int ss = 4;   // suréchantillonnage (bords lissés)
    for (int y = n - 1; y >= 0; --y)
        for (int x = 0; x < n; ++x) {
            double R = 0, G = 0, B = 0, A = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const double u = ((x + (sx + 0.5) / ss) / n) * 2 - 1, v = ((y + (sy + 0.5) / ss) / n) * 2 - 1;
                    double r, g, b, a;
                    shade(u, v, r, g, b, a);
                    R += r * a; G += g * a; B += b * a; A += a;
                }
            const double k = A > 0 ? 1.0 / A : 0;
            px.push_back(static_cast<uint8_t>(B * k));
            px.push_back(static_cast<uint8_t>(G * k));
            px.push_back(static_cast<uint8_t>(R * k));
            px.push_back(static_cast<uint8_t>(std::lround(A / (ss * ss) * 255)));
        }
    std::vector<uint8_t> out;
    put32(out, 40); put32(out, static_cast<uint32_t>(n)); put32(out, static_cast<uint32_t>(n * 2));
    put16(out, 1); put16(out, 32); put32(out, 0); put32(out, static_cast<uint32_t>(px.size()));
    put32(out, 0); put32(out, 0); put32(out, 0); put32(out, 0);
    out.insert(out.end(), px.begin(), px.end());
    const int row = ((n + 31) / 32) * 4;
    out.insert(out.end(), static_cast<size_t>(row * n), 0);   // masque : tout visible (alpha décide)
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    const int sizes[] = {16, 32, 48, 256};
    std::vector<std::vector<uint8_t>> imgs;
    for (int s : sizes) imgs.push_back(image(s));
    std::vector<uint8_t> f;
    put16(f, 0); put16(f, 1); put16(f, static_cast<uint16_t>(imgs.size()));
    uint32_t offset = 6 + 16 * static_cast<uint32_t>(imgs.size());
    for (size_t i = 0; i < imgs.size(); ++i) {
        const int s = sizes[i];
        f.push_back(static_cast<uint8_t>(s >= 256 ? 0 : s));
        f.push_back(static_cast<uint8_t>(s >= 256 ? 0 : s));
        f.push_back(0); f.push_back(0);
        put16(f, 1); put16(f, 32);
        put32(f, static_cast<uint32_t>(imgs[i].size()));
        put32(f, offset);
        offset += static_cast<uint32_t>(imgs[i].size());
    }
    for (auto& im : imgs) f.insert(f.end(), im.begin(), im.end());
    FILE* out = nullptr;
    if (fopen_s(&out, argv[1], "wb") != 0 || !out) return 1;
    fwrite(f.data(), 1, f.size(), out);
    fclose(out);
    return 0;
}
