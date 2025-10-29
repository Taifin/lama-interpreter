#include "bytefile.h"
#include "../runtime/runtime_common.h"
#include "../runtime/runtime.h"

/* Gets a string from a string table by an index */
char *get_string(bytefile *f, int pos) {
    return &f->string_ptr[pos];
}

/* Gets a name for a public symbol */
char *get_public_name(bytefile *f, int i) {
    return get_string(f, f->public_ptr[i * 2]);
}

/* Gets an offset for a publie symbol */
int get_public_offset(bytefile *f, int i) {
    return f->public_ptr[i * 2 + 1];
}

bytefile *readFile(const std::string &filename) {
    FILE *f = fopen(filename.c_str(), "rb");
    long size;
    bytefile *bf{};

    if (f == nullptr) {
        failure("%s\n", strerror(errno));
    }

    if (fseek(f, 0, SEEK_END) == -1) {
        failure("%s\n", strerror(errno));
    }

    bf = static_cast<bytefile *>(malloc(sizeof(bytefile) + (size = ftell(f))));

    if (bf == nullptr) {
        failure("*** FAILURE: unable to allocate memory.\n");
        return nullptr;
    }

    rewind(f);

    if (size != fread(&bf->stringtab_size, 1, size, f)) {
        failure("%s\n", strerror(errno));
    }

    fclose(f);

    bf->string_ptr = &bf->buffer[bf->public_symbols_number * 2 * sizeof(int)];
    bf->public_ptr = (int *) bf->buffer;
    bf->code_ptr = &bf->string_ptr[bf->stringtab_size];
    bf->global_ptr = (int *) malloc(bf->global_area_size * sizeof(int));

    return bf;
}
