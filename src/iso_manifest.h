#pragma once
struct MclaIsoEntry { const char* name; long long offset; long long size; };
static const MclaIsoEntry kMclaIsoManifest[] = {
    {"default.xex", 316166144LL, 9252864LL},
    {"intro720.bik", 6626277376LL, 123836692LL},
    {"intro576_16x9.bik", 6378610688LL, 123833536LL},
    {"intro576_4x3.bik", 6502445056LL, 123831648LL},
    {"attract720.bik", 6315442176LL, 63168256LL},
    {"attract576_16x9.bik", 6813253632LL, 63140548LL},
    {"attract576_4x3.bik", 6750115840LL, 63136172LL},
    {"nxeart", 6876395520LL, 1425408LL},
    {"xarchive_audlo.rpf", 325419008LL, 1463189504LL},
    {"xarchive_audio.rpf", 3919380480LL, 1615757312LL},
    {"xarchive_music.rpf", 5535137792LL, 780304384LL},
    {"xarchive_cache.rpf", 1788608512LL, 2130739200LL},
};
static const int kMclaIsoManifestCount = 12;
