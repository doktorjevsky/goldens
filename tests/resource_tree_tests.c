#include <windows.h>
#include <commctrl.h>

#include <stdio.h>

#include "../src/resource_tree.h"

static HTREEITEM insert_item(HWND tree, HTREEITEM parent,
                            const wchar_t *label, ResourceTreeNode *node) {
    TVINSERTSTRUCTW insert = {0};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = (wchar_t *)label;
    insert.item.lParam = (LPARAM)node;
    return TreeView_InsertItem(tree, &insert);
}

int main(void) {
    INITCOMMONCONTROLSEX controls = {
        sizeof(controls), ICC_TREEVIEW_CLASSES
    };
    if (!InitCommonControlsEx(&controls)) {
        fprintf(stderr, "Could not initialize common controls.\n");
        return 1;
    }
    HWND parent = CreateWindowExW(0, L"STATIC", L"resource-tree-test",
                                  WS_OVERLAPPED, 0, 0, 100, 100,
                                  NULL, NULL, GetModuleHandleW(NULL), NULL);
    HWND tree = parent ? CreateWindowExW(
        0, WC_TREEVIEWW, NULL, WS_CHILD, 0, 0, 100, 100,
        parent, NULL, GetModuleHandleW(NULL), NULL) : NULL;
    if (!tree) {
        if (parent) DestroyWindow(parent);
        fprintf(stderr, "Could not create resource tree.\n");
        return 1;
    }

    wchar_t root_path[] = L"C:\\goldens";
    wchar_t png_path[] = L"C:\\goldens\\image.png";
    ResourceTreeNode root = {RESOURCE_DIRECTORY, root_path, -1};
    ResourceTreeNode png = {RESOURCE_PNG, png_path, -1};
    ResourceTreeNode annotation = {RESOURCE_ANNOTATION, NULL, 0};
    HTREEITEM root_item = insert_item(tree, TVI_ROOT, L"goldens", &root);
    HTREEITEM png_item = insert_item(tree, root_item, L"image.png", &png);
    HTREEITEM annotation_item = insert_item(
        tree, png_item, L"annotation", &annotation);

    int failed = !root_item || !png_item || !annotation_item ||
        golden_resource_tree_selected_directory(tree) != NULL;
    TreeView_SelectItem(tree, root_item);
    failed |= golden_resource_tree_selected_directory(tree) != root_path;
    failed |= golden_resource_tree_destination_directory(
        tree, L"C:\\fallback") != root_path;
    TreeView_SelectItem(tree, png_item);
    failed |= golden_resource_tree_selected_directory(tree) != root_path;
    TreeView_SelectItem(tree, annotation_item);
    failed |= golden_resource_tree_selected_directory(tree) != root_path;

    /* Regression: clearing selection must not retain the previous directory. */
    TreeView_SelectItem(tree, NULL);
    failed |= golden_resource_tree_selected_directory(tree) != NULL;
    failed |= golden_resource_tree_destination_directory(tree, root_path) !=
              root_path;

    DestroyWindow(tree);
    DestroyWindow(parent);
    if (failed) {
        fprintf(stderr, "Resource tree selection tests failed.\n");
        return 1;
    }
    printf("All Goldens resource tree selection tests passed.\n");
    return 0;
}
