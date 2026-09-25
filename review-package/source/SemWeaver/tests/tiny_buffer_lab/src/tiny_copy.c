#include <stddef.h>
#include <string.h>

typedef struct {
    char tag[8];
} Record;

int save_tag(Record *record, const char *input, size_t len) {
    if (!record || !input) {
        return -1;
    }

    memcpy(record->tag, input, len);
    record->tag[len] = '\0';
    return 0;
}

