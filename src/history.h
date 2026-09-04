#ifndef GOLDENS_HISTORY_H
#define GOLDENS_HISTORY_H

#include <windows.h>

#include "model.h"

#define GOLDEN_HISTORY_LIMIT 32
#define GOLDEN_HISTORY_PATH_CAPACITY (MAX_PATH * 4)

typedef enum {
    GOLDEN_HISTORY_ANNOTATIONS,
    GOLDEN_HISTORY_CREATE_DIRECTORY,
    GOLDEN_HISTORY_MOVE_PNG,
    GOLDEN_HISTORY_MOVE_DIRECTORY,
    GOLDEN_HISTORY_CREATE_PNG,
    GOLDEN_HISTORY_DELETE_PNG,
    GOLDEN_HISTORY_DELETE_DIRECTORY,
    GOLDEN_HISTORY_REPLACE_MOVE_PNG,
    GOLDEN_HISTORY_RECAPTURE_PNG
} GoldenHistoryKind;

typedef struct {
    GoldenHistoryKind kind;
    Annotation *annotations;
    int annotation_count;
    int selected_annotation;
    wchar_t source[GOLDEN_HISTORY_PATH_CAPACITY];
    wchar_t destination[GOLDEN_HISTORY_PATH_CAPACITY];
    wchar_t auxiliary[GOLDEN_HISTORY_PATH_CAPACITY];
    BOOL staged;
} GoldenHistoryEntry;

typedef void (*GoldenHistoryDiscard)(GoldenHistoryEntry *entry, void *context);

typedef struct {
    GoldenHistoryEntry entries[GOLDEN_HISTORY_LIMIT];
    int start;
    int count;
} GoldenHistoryStack;

typedef struct {
    GoldenHistoryStack undo;
    GoldenHistoryStack redo;
    GoldenHistoryDiscard discard;
    void *discard_context;
} GoldenHistory;

void golden_history_init(GoldenHistory *history,
                         GoldenHistoryDiscard discard, void *context);
void golden_history_destroy(GoldenHistory *history);
void golden_history_clear(GoldenHistory *history);
BOOL golden_history_entry_annotations(GoldenHistoryEntry *entry,
                                      const Annotation *annotations, int count,
                                      int selected);
void golden_history_entry_resource(GoldenHistoryEntry *entry,
                                   GoldenHistoryKind kind,
                                   const wchar_t *source,
                                   const wchar_t *destination,
                                   const wchar_t *auxiliary);
void golden_history_entry_dispose(GoldenHistoryEntry *entry);
void golden_history_push_new(GoldenHistory *history, GoldenHistoryEntry *entry);
BOOL golden_history_pop_undo(GoldenHistory *history, GoldenHistoryEntry *entry);
BOOL golden_history_pop_redo(GoldenHistory *history, GoldenHistoryEntry *entry);
void golden_history_restore_undo(GoldenHistory *history, GoldenHistoryEntry *entry);
void golden_history_restore_redo(GoldenHistory *history, GoldenHistoryEntry *entry);
void golden_history_transfer_to_undo(GoldenHistory *history, GoldenHistoryEntry *entry);
void golden_history_transfer_to_redo(GoldenHistory *history, GoldenHistoryEntry *entry);
void golden_history_remove_annotations(GoldenHistory *history);
BOOL golden_history_can_undo(const GoldenHistory *history);
BOOL golden_history_can_redo(const GoldenHistory *history);
BOOL golden_history_annotations_equal(const Annotation *left, int left_count,
                                      const Annotation *right, int right_count);

#endif
