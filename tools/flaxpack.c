// ===========================================================================
// flaxpack - offline map pipeline tool for the Flax Engine.
//
// The "compiler" half of the source -> compiler -> runtime split (see
// src/io/flaxmap.h for both format specs):
//
//   flaxpack pack   <in.map>  <out.fmap>   compile text source to baked binary
//   flaxpack unpack <in.fmap> <out.map>    decompile binary back to text
//   flaxpack dump   <in.fmap>              print header, lump table, contents
//
// pack recompiles portal links from scratch; unpack drops them (they are
// derived data and would just go stale in a hand-edited file). dump is the
// debugging path: it shows the raw container before decoding anything, so a
// corrupt file still tells you which lump went wrong.
// ===========================================================================

#include "io/flaxmap.h"
#include "world/world.h"
#include <stdio.h>
#include <string.h>

static int Usage(void) {
    fprintf(stderr,
        "usage: flaxpack pack   <in.map>  <out.fmap>\n"
        "       flaxpack unpack <in.fmap> <out.map>\n"
        "       flaxpack dump   <in.fmap>\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) return Usage();

    if (strcmp(argv[1], "pack") == 0 && argc == 4) {
        if (!MapSourceLoad(argv[2])) { fprintf(stderr, "flaxpack: failed to load %s\n", argv[2]); return 1; }
        if (!MapBakedSave(argv[3]))  { fprintf(stderr, "flaxpack: failed to write %s\n", argv[3]); return 1; }
        printf("packed %s -> %s  (%d sectors, %d walls, %d vertices, %d textures)\n",
               argv[2], argv[3], sector_counter, wall_counter, vertex_counter, texname_counter);
        return 0;
    }
    if (strcmp(argv[1], "unpack") == 0 && argc == 4) {
        if (!MapBakedLoad(argv[2]))  { fprintf(stderr, "flaxpack: failed to load %s\n", argv[2]); return 1; }
        if (!MapSourceSave(argv[3])) { fprintf(stderr, "flaxpack: failed to write %s\n", argv[3]); return 1; }
        printf("unpacked %s -> %s\n", argv[2], argv[3]);
        return 0;
    }
    if (strcmp(argv[1], "dump") == 0 && argc == 3) {
        return MapBakedDump(argv[2], stdout) ? 0 : 1;
    }
    return Usage();
}
