#ifndef GOLDENS_RESOURCE_TREE_H
#define GOLDENS_RESOURCE_TREE_H

#include <windows.h>
#include <commctrl.h>

typedef enum {
    RESOURCE_DIRECTORY,
    RESOURCE_PNG,
    RESOURCE_ANNOTATION
} ResourceNodeKind;

typedef struct {
    ResourceNodeKind kind;
    wchar_t *path;
    int annotation_index;
} ResourceTreeNode;

const wchar_t *golden_resource_tree_selected_directory(HWND tree);
const wchar_t *golden_resource_tree_destination_directory(
    HWND tree, const wchar_t *default_directory);

#endif
