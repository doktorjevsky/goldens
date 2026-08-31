#include "resource_tree.h"

static ResourceTreeNode *node_data(HWND tree, HTREEITEM item) {
    if (!tree || !item) return NULL;
    TVITEMW info = {0};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    return TreeView_GetItem(tree, &info) ?
        (ResourceTreeNode *)info.lParam : NULL;
}

const wchar_t *golden_resource_tree_selected_directory(HWND tree) {
    HTREEITEM item = tree ? TreeView_GetSelection(tree) : NULL;
    ResourceTreeNode *node = node_data(tree, item);
    if (!node) return NULL;
    if (node->kind == RESOURCE_DIRECTORY)
        return node->path;
    if (node->kind == RESOURCE_ANNOTATION)
        item = TreeView_GetParent(tree, item);
    item = item ? TreeView_GetParent(tree, item) : NULL;
    node = node_data(tree, item);
    return node && node->kind == RESOURCE_DIRECTORY ? node->path : NULL;
}

const wchar_t *golden_resource_tree_destination_directory(
    HWND tree, const wchar_t *default_directory) {
    const wchar_t *selected = golden_resource_tree_selected_directory(tree);
    return selected && selected[0] ? selected : default_directory;
}
