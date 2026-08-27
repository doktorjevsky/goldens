#include "history.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void copy_path(wchar_t *destination, const wchar_t *source) {
    wcsncpy(destination, source ? source : L"", GOLDEN_HISTORY_PATH_CAPACITY - 1);
    destination[GOLDEN_HISTORY_PATH_CAPACITY - 1] = 0;
}

void golden_history_entry_dispose(GoldenHistoryEntry *entry) {
    if (!entry) return;
    free(entry->annotations);
    ZeroMemory(entry, sizeof(*entry));
}

BOOL golden_history_entry_annotations(GoldenHistoryEntry *entry,
                                      const Annotation *annotations, int count,
                                      int selected) {
    if (!entry || count < 0 || count > MAX_ANNOTATIONS ||
        (count && !annotations)) return FALSE;
    ZeroMemory(entry, sizeof(*entry));
    entry->kind = GOLDEN_HISTORY_ANNOTATIONS;
    entry->annotation_count = count;
    entry->selected_annotation = selected;
    if (!count) return TRUE;
    entry->annotations = (Annotation *)malloc(sizeof(Annotation) * (size_t)count);
    if (!entry->annotations) return FALSE;
    memcpy(entry->annotations, annotations, sizeof(Annotation) * (size_t)count);
    return TRUE;
}

void golden_history_entry_resource(GoldenHistoryEntry *entry,
                                   GoldenHistoryKind kind,
                                   const wchar_t *source,
                                   const wchar_t *destination,
                                   const wchar_t *auxiliary) {
    if (!entry) return;
    ZeroMemory(entry, sizeof(*entry));
    entry->kind = kind;
    copy_path(entry->source, source);
    copy_path(entry->destination, destination);
    copy_path(entry->auxiliary, auxiliary);
}

static void discard_entry(GoldenHistory *history, GoldenHistoryEntry *entry) {
    if (history->discard) history->discard(entry, history->discard_context);
    golden_history_entry_dispose(entry);
}

static void clear_stack(GoldenHistory *history, GoldenHistoryStack *stack) {
    for (int i = 0; i < stack->count; ++i) {
        int index = (stack->start + i) % GOLDEN_HISTORY_LIMIT;
        discard_entry(history, &stack->entries[index]);
    }
    stack->start = 0;
    stack->count = 0;
}

void golden_history_init(GoldenHistory *history,
                         GoldenHistoryDiscard discard, void *context) {
    if (!history) return;
    ZeroMemory(history, sizeof(*history));
    history->discard = discard;
    history->discard_context = context;
}

void golden_history_clear(GoldenHistory *history) {
    if (!history) return;
    clear_stack(history, &history->undo);
    clear_stack(history, &history->redo);
}

void golden_history_destroy(GoldenHistory *history) {
    golden_history_clear(history);
}

static void push_stack(GoldenHistory *history, GoldenHistoryStack *stack,
                       GoldenHistoryEntry *entry) {
    if (stack->count == GOLDEN_HISTORY_LIMIT) {
        discard_entry(history, &stack->entries[stack->start]);
        stack->start = (stack->start + 1) % GOLDEN_HISTORY_LIMIT;
        --stack->count;
    }
    int index = (stack->start + stack->count) % GOLDEN_HISTORY_LIMIT;
    stack->entries[index] = *entry;
    ZeroMemory(entry, sizeof(*entry));
    ++stack->count;
}

static BOOL pop_stack(GoldenHistoryStack *stack, GoldenHistoryEntry *entry) {
    if (!stack->count || !entry) return FALSE;
    int index = (stack->start + stack->count - 1) % GOLDEN_HISTORY_LIMIT;
    *entry = stack->entries[index];
    ZeroMemory(&stack->entries[index], sizeof(stack->entries[index]));
    --stack->count;
    if (!stack->count) stack->start = 0;
    return TRUE;
}

void golden_history_push_new(GoldenHistory *history, GoldenHistoryEntry *entry) {
    if (!history || !entry) return;
    clear_stack(history, &history->redo);
    push_stack(history, &history->undo, entry);
}

BOOL golden_history_pop_undo(GoldenHistory *history, GoldenHistoryEntry *entry) {
    return history && pop_stack(&history->undo, entry);
}

BOOL golden_history_pop_redo(GoldenHistory *history, GoldenHistoryEntry *entry) {
    return history && pop_stack(&history->redo, entry);
}

void golden_history_restore_undo(GoldenHistory *history, GoldenHistoryEntry *entry) {
    if (history && entry) push_stack(history, &history->undo, entry);
}

void golden_history_restore_redo(GoldenHistory *history, GoldenHistoryEntry *entry) {
    if (history && entry) push_stack(history, &history->redo, entry);
}

void golden_history_transfer_to_undo(GoldenHistory *history, GoldenHistoryEntry *entry) {
    golden_history_restore_undo(history, entry);
}

void golden_history_transfer_to_redo(GoldenHistory *history, GoldenHistoryEntry *entry) {
    golden_history_restore_redo(history, entry);
}

static void remove_annotations_from_stack(GoldenHistory *history,
                                          GoldenHistoryStack *stack) {
    GoldenHistoryStack kept = {0};
    for (int i = 0; i < stack->count; ++i) {
        int index = (stack->start + i) % GOLDEN_HISTORY_LIMIT;
        GoldenHistoryEntry entry = stack->entries[index];
        ZeroMemory(&stack->entries[index], sizeof(stack->entries[index]));
        if (entry.kind == GOLDEN_HISTORY_ANNOTATIONS)
            discard_entry(history, &entry);
        else
            push_stack(history, &kept, &entry);
    }
    *stack = kept;
}

void golden_history_remove_annotations(GoldenHistory *history) {
    if (!history) return;
    remove_annotations_from_stack(history, &history->undo);
    remove_annotations_from_stack(history, &history->redo);
}

BOOL golden_history_can_undo(const GoldenHistory *history) {
    return history && history->undo.count > 0;
}

BOOL golden_history_can_redo(const GoldenHistory *history) {
    return history && history->redo.count > 0;
}

BOOL golden_history_annotations_equal(const Annotation *left, int left_count,
                                      const Annotation *right, int right_count) {
    if (left_count < 0 || right_count < 0 || left_count != right_count ||
        (left_count && (!left || !right))) return FALSE;
    return !left_count ||
        memcmp(left, right, sizeof(Annotation) * (size_t)left_count) == 0;
}
