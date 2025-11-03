#include "bytefile.h"
#include "../runtime/runtime_common.h"
#include "../runtime/runtime.h"

/* Gets a string from a string table by an index */
char *get_string(bytefile *f, const int pos) {
    if (pos < 0 || pos > f->stringtab_size) {
        failure("Invalid string requested %d", pos);
    }
    return &f->string_ptr[pos];
}

/* Gets a name for a public symbol */
char *get_public_name(bytefile *f, const int i) {
    if (i < 0 || i > f->public_symbols_number) {
        failure("Invalid public symbol requested %d", i);
    }
    return get_string(f, f->public_ptr[i * 2]);
}

/* Gets an offset for a public symbol */
int get_public_offset(bytefile *f, const int i) {
    if (i < 0 || i > f->public_symbols_number) {
        failure("Invalid public symbol requested %d", i);
    }
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
    }

    rewind(f);

    if (size != fread(&bf->stringtab_size, 1, size, f)) {
        failure("%s\n", strerror(errno));
    }

    fclose(f);

    if (bf->global_area_size < 0) {
        failure("Incorrect bytecode file format: negative global area size");
    }

    if (bf->stringtab_size < 0) {
        failure("Incorrect bytecode file format: negative string table size");
    }

    if (bf->public_symbols_number < 0) {
        failure("Incorrect bytecode file format: negative number of public symbols");
    }

    if (bf->public_symbols_number * 2 * sizeof(int) + bf->stringtab_size > size) {
        failure("Incorrect bytecode file format: insufficient string or public section");
    }

    bf->string_ptr = &bf->buffer[bf->public_symbols_number * 2 * sizeof(int)];
    bf->public_ptr = (int *) bf->buffer;
    bf->code_ptr = &bf->string_ptr[bf->stringtab_size];
    bf->global_ptr = (int *) malloc(bf->global_area_size * sizeof(int));
    bf->size = size - ((long)bf->code_ptr - (long)&bf->stringtab_size);

    bf->entrypoint_ptr = nullptr;
    for (int i = 0; i < bf->public_symbols_number; i++) {
        auto public_name = get_public_name(bf, i);
        if (std::strcmp(public_name, "main") == 0) {
            auto main_offset = get_public_offset(bf, i);
            if (main_offset < 0 || main_offset >= size) {
                failure("Incorrect bytecode file format: entrypoint address is not in range");
            }

            bf->entrypoint_ptr = bf->code_ptr + main_offset;
        }
    }

    if (bf->entrypoint_ptr == nullptr) {
        failure("Incorrect bytecode file format: entrypoint not found");
    }

    return bf;
}
