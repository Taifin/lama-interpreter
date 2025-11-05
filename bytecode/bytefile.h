#ifndef VIRTUAL_MACHINES_BYTEFILE_H
#define VIRTUAL_MACHINES_BYTEFILE_H
#include <string>

struct bytefile {
    long code_size;
    char *entrypoint_ptr;
    char *string_ptr; /* A pointer to the beginning of the string table */
    int *public_ptr; /* A pointer to the beginning of publics table    */
    char *code_ptr; /* A pointer to the bytefile itself               */
    int *global_ptr; /* A pointer to the global area                   */
    int stringtab_size; /* The size (in bytes) of the string table        */
    int global_area_size; /* The size (in words) of global area             */
    int public_symbols_number; /* The number of public symbols                   */
    char buffer[0];
};

/* Gets a string from a string table by an index */
char *get_string(bytefile *f, int pos);

/* Gets a name for a public symbol */
char *get_public_name(bytefile *f, int i);

/* Gets an offset for a public symbol */
int get_public_offset(bytefile *f, int i);

bytefile *readFile(const std::string &filename);

#endif //VIRTUAL_MACHINES_BYTEFILE_H
