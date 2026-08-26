#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <errno.h>

#include "model.h"
#include "document.h"
#include "window_tracker.h"
#include "image_io.h"
#include "preview_capture.h"
#include "preview_service.h"
#include "editor_render.h"
#include "ui_layout.h"
#include "ui_tooltip.h"
#include "ui_tool_icon.h"
#include "resource_ops.h"
#include "atomic_file.h"

#define APP_NAME L"Goldens"
#define MAX_HISTORY 32
#define WINDOW_TIMER 1
#define PREVIEW_TIMER 2
#define PREVIEW_INTERVAL_MS 80
#define EDITOR_TOOLTIP_TIMER 3
#define EDITOR_TOOLTIP_DELAY_MS 450
#define WM_PREVIEW_READY (WM_APP + 1)
#define WM_RESOURCE_RENAMED (WM_APP + 2)
#define WM_ANNOTATION_RENAMED (WM_APP + 3)
#define WM_BEGIN_TREE_RENAME (WM_APP + 4)

enum {
    ID_OPEN = 100, ID_SAVE, ID_EXIT, ID_UNDO, ID_REDO, ID_RENAME, ID_DELETE,
    ID_CLEAR_CLICK, ID_FIT, ID_ZOOM_OUT, ID_ZOOM_IN, ID_ACTUAL,
    ID_CAPTURE, ID_RECAPTURE, ID_REFRESH,
    ID_TOOL_SELECT, ID_TOOL_RECTANGLE, ID_TOOL_CLICK,
    ID_TREE = 200, ID_EDITOR, ID_WINDOWS, ID_SPLITTER_LEFT, ID_SPLITTER_RIGHT,
    ID_PROMPT_EDIT = 300, ID_PROMPT_OK, ID_PROMPT_CANCEL
};

typedef enum {
    TOOL_SELECT,
    TOOL_RECTANGLE,
    TOOL_CLICK
} ToolMode;

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

typedef struct {
    Annotation items[MAX_ANNOTATIONS];
    int count;
    int selected;
} Snapshot;

typedef struct {
    HWND window;
    HWND edit;
    wchar_t *output;
    size_t capacity;
    BOOL accepted;
    BOOL done;
} PromptState;

typedef struct {
    HINSTANCE instance;
    HWND main, tree, editor, windows, status;
    HWND editor_tooltip, tool_tooltip;
    HWND left_splitter, right_splitter;
    HWND context_label;
    HWND tool_buttons[GOLDEN_TOOL_BUTTON_COUNT];
    HWND view_buttons[GOLDEN_VIEW_BUTTON_COUNT];
    HWND window_buttons[2];
    HMENU capture_menu;
    TOOLINFOW tool_button_tooltips[GOLDEN_TOOL_BUTTON_COUNT];
    IWICImagingFactory *wic;

    wchar_t root[MAX_PATH * 4];
    wchar_t image_path[MAX_PATH * 4];
    wchar_t current_dir[MAX_PATH * 4];
    BYTE *pixels;
    UINT image_w, image_h, stride;
    uint64_t image_revision;
    GoldenBackBuffer editor_buffer;
    GoldenImageCache image_cache;

    GoldenImage preview_image;
    GoldenPreviewService preview_service;
    HWND preview_target;
    wchar_t preview_title[256];
    BOOL preview_mode;
    BOOL resource_visible;
    BOOL preview_loading;
    LONG preview_generation;

    double zoom;
    int pan_x, pan_y;
    BOOL panning;
    POINT pan_start;
    int pan_origin_x, pan_origin_y;

    Annotation annotations[MAX_ANNOTATIONS];
    int annotation_count;
    int selected;
    BOOL dirty;
    Snapshot undo[MAX_HISTORY], redo[MAX_HISTORY];
    int undo_count, redo_count;
    int drag_mode;
    POINT drag_start;
    RECT drag_original;
    BOOL drawing;
    POINT draw_start, draw_current;
    ToolMode tool;

    GoldenWindowInfo window_items[MAX_WINDOWS];
    int window_count;
    BOOL rebuilding_windows;
    BOOL rebuilding_resources;
    wchar_t pending_resource_selection[MAX_PATH * 4];

    int left_column_width;
    int right_column_width;
    BOOL left_collapsed, right_collapsed;
    BOOL splitter_dragging;
    POINT splitter_drag_start;
    int splitter_width_start;

    int tooltip_pending;
    int tooltip_visible;
    BOOL editor_mouse_tracking;
    wchar_t tooltip_text[128];
    TOOLINFOW editor_tooltip_tool;
    int hovered_tool;
} GoldenAppState;

static GoldenAppState g;

static void initialize_app_state(HINSTANCE instance) {
    g.instance = instance;
    g.selected = -1;
    g.tool = TOOL_SELECT;
    g.left_column_width = GOLDEN_RESOURCE_PANE_DEFAULT;
    g.right_column_width = GOLDEN_WINDOWS_PANE_DEFAULT;
    g.tooltip_pending = -1;
    g.tooltip_visible = -1;
    g.hovered_tool = -1;
}

static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK EditorProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK PromptProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SplitterProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ToolButtonProc(HWND, UINT, WPARAM, LPARAM,
                                       UINT_PTR, DWORD_PTR);
static BOOL prompt_text(HWND owner, const wchar_t *title, const wchar_t *label,
                        wchar_t *value, size_t capacity);
static void preview_window(HWND target);
static void request_preview_frame(void);
static HWND selected_capture_window(void);
static void refresh_annotation_tree(void);
static void update_context_label(void);
static void zoom_by(double factor, const POINT *anchor);
static void update_tool_availability(void);
static void update_capture_availability(void);
static void set_tool_with_focus(ToolMode tool, BOOL focus_editor);
static void set_tool(ToolMode tool);
static void layout_children(HWND hwnd);
static void hide_annotation_tooltip(HWND hwnd);
static void update_annotation_hover(HWND hwnd, POINT client);
static void draw_tool_button(const DRAWITEMSTRUCT *item);

static void show_error(const wchar_t *message) {
    MessageBoxW(g.main, message, APP_NAME, MB_OK | MB_ICONERROR);
}

static wchar_t *dup_wide(const wchar_t *value) {
    size_t bytes = (wcslen(value) + 1) * sizeof(wchar_t);
    wchar_t *copy = (wchar_t *)malloc(bytes);
    if (copy) memcpy(copy, value, bytes);
    return copy;
}

static BOOL ends_with_png(const wchar_t *path) {
    size_t n = wcslen(path);
    return n >= 4 && _wcsicmp(path + n - 4, L".png") == 0;
}

static BOOL json_path_for(const wchar_t *png, wchar_t *out, size_t cap) {
    return golden_resource_json_path(png, out, cap);
}

static BOOL parent_dir_for(const wchar_t *path, wchar_t *out, size_t cap) {
    return golden_path_copy(path, out, cap) && PathRemoveFileSpecW(out);
}

static void update_status(void) {
    if (!g.status) return;
    wchar_t text[MAX_PATH * 4 + 64];
    const wchar_t *folder = g.current_dir[0] ? g.current_dir : g.root;
    _snwprintf(text, _countof(text), L"  Capture folder: %s", folder[0] ? folder : L"(unavailable)");
    SetWindowTextW(g.status, text);
}

static void update_context_label(void) {
    if (!g.context_label) return;
    wchar_t text[MAX_PATH * 4 + 64];
    if (g.preview_mode)
        _snwprintf(text, _countof(text), L"  Previewing window  —  %s",
                   g.preview_title[0] ? g.preview_title : L"Untitled window");
    else if (g.resource_visible && g.image_path[0])
        _snwprintf(text, _countof(text), L"  Editing resource  —  %s",
                   PathFindFileNameW(g.image_path));
    else
        wcscpy(text, L"  No resource selected");
    SetWindowTextW(g.context_label, text);
}

static void snapshot_current(Snapshot *s) {
    s->count = g.annotation_count;
    s->selected = g.selected;
    memcpy(s->items, g.annotations,
           sizeof(Annotation) * (size_t)g.annotation_count);
}

static void restore_snapshot(const Snapshot *s) {
    g.annotation_count = s->count;
    g.selected = s->selected;
    memcpy(g.annotations, s->items,
           sizeof(Annotation) * (size_t)s->count);
    g.dirty = TRUE;
    update_tool_availability();
    refresh_annotation_tree();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void push_undo(void) {
    if (g.undo_count == MAX_HISTORY) {
        memmove(&g.undo[0], &g.undo[1], sizeof(Snapshot) * (MAX_HISTORY - 1));
        g.undo_count--;
    }
    snapshot_current(&g.undo[g.undo_count++]);
    g.redo_count = 0;
}

static void undo_action(void) {
    if (!g.undo_count) return;
    if (g.redo_count < MAX_HISTORY) snapshot_current(&g.redo[g.redo_count++]);
    restore_snapshot(&g.undo[--g.undo_count]);
}

static void redo_action(void) {
    if (!g.redo_count) return;
    if (g.undo_count < MAX_HISTORY) snapshot_current(&g.undo[g.undo_count++]);
    restore_snapshot(&g.redo[--g.redo_count]);
}

static BOOL annotation_name_exists(const wchar_t *name, int except) {
    return golden_name_exists(g.annotations, g.annotation_count, name, except);
}

static void make_unique_name(wchar_t *out, size_t cap) {
    golden_make_unique_name(g.annotations, g.annotation_count, out, cap);
}

static void trim_text(wchar_t *text) {
    wchar_t *start = text;
    while (*start && iswspace(*start)) ++start;
    if (start != text) memmove(text, start, (wcslen(start) + 1) * sizeof(wchar_t));
    size_t length = wcslen(text);
    while (length && iswspace(text[length - 1])) text[--length] = 0;
}

static BOOL prompt_annotation_name(wchar_t *name, size_t capacity, int except) {
    for (;;) {
        if (!prompt_text(g.main, except < 0 ? L"New annotation" : L"Rename annotation",
                         L"Unique annotation name:", name, capacity)) return FALSE;
        trim_text(name);
        if (!name[0]) {
            show_error(L"The annotation name cannot be empty.");
            continue;
        }
        if (annotation_name_exists(name, except)) {
            show_error(L"That annotation name is already used in this image.");
            continue;
        }
        return TRUE;
    }
}

static BOOL prompt_text(HWND owner, const wchar_t *title, const wchar_t *label,
                        wchar_t *value, size_t capacity) {
    static BOOL registered;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = g.instance;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"GoldensPrompt";
        RegisterClassW(&wc);
        registered = TRUE;
    }
    PromptState state = {0};
    state.output = value;
    state.capacity = capacity;
    RECT r = {0, 0, 430, 155};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT owner_rect;
    GetWindowRect(owner, &owner_rect);
    int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - (r.right - r.left)) / 2;
    int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - (r.bottom - r.top)) / 2;
    state.window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"GoldensPrompt", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, r.right - r.left, r.bottom - r.top,
        owner, NULL, g.instance, &state);
    CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE, 16, 14, 390, 20,
                  state.window, NULL, g.instance, NULL);
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 38, 390, 25,
        state.window, (HMENU)ID_PROMPT_EDIT, g.instance, NULL);
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                  236, 79, 80, 28, state.window, (HMENU)ID_PROMPT_OK, g.instance, NULL);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                  326, 79, 80, 28, state.window, (HMENU)ID_PROMPT_CANCEL, g.instance, NULL);
    SendMessageW(state.edit, EM_SETSEL, 0, -1);
    EnableWindow(owner, FALSE);
    ShowWindow(state.window, SW_SHOW);
    SetFocus(state.edit);
    MSG msg;
    while (!state.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd == state.edit && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(state.window, WM_COMMAND, ID_PROMPT_OK, 0);
            continue;
        }
        if (msg.hwnd == state.edit && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(state.window, WM_COMMAND, ID_PROMPT_CANCEL, 0);
            continue;
        }
        if (!IsDialogMessageW(state.window, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return state.accepted;
}

static LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PromptState *state = (PromptState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        state = (PromptState *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);
    }
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_PROMPT_OK) {
            GetWindowTextW(state->edit, state->output, (int)state->capacity);
            state->accepted = TRUE;
            state->done = TRUE;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == ID_PROMPT_CANCEL) {
            state->done = TRUE;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        state->done = TRUE;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void clear_image(void) {
    free(g.pixels);
    g.pixels = NULL;
    g.image_w = g.image_h = g.stride = 0;
    g.resource_visible = FALSE;
    ++g.image_revision;
}

static void clear_preview(void) {
    InterlockedIncrement(&g.preview_generation);
    golden_preview_service_clear(&g.preview_service);
    g.preview_image = (GoldenImage){0};
    g.preview_target = NULL;
    g.preview_title[0] = 0;
    g.preview_mode = FALSE;
    g.preview_loading = FALSE;
    ++g.image_revision;
    update_context_label();
    update_tool_availability();
}

static BYTE *active_pixels(void) {
    return g.preview_mode ? g.preview_image.pixels : g.resource_visible ? g.pixels : NULL;
}
static UINT active_width(void) {
    return g.preview_mode ? g.preview_image.width : g.resource_visible ? g.image_w : 0;
}
static UINT active_height(void) {
    return g.preview_mode ? g.preview_image.height : g.resource_visible ? g.image_h : 0;
}

static BOOL load_png(const wchar_t *path) {
    GoldenImage image = {0};
    if (!golden_png_load(g.wic, path, &image)) return FALSE;
    clear_image();
    g.pixels = image.pixels;
    g.image_w = image.width;
    g.image_h = image.height;
    g.stride = image.stride;
    return TRUE;
}

static BOOL save_png_pixels(const wchar_t *path, BYTE *pixels, UINT width, UINT height, UINT stride) {
    return golden_png_save(g.wic, path, pixels, width, height, stride);
}

static void load_annotations(const wchar_t *png_path) {
    g.annotation_count = 0;
    g.selected = -1;
    g.dirty = FALSE;
    g.undo_count = g.redo_count = 0;
    wchar_t path[MAX_PATH * 4];
    if (!json_path_for(png_path, path, _countof(path))) {
        show_error(L"The resource path is too long to locate its annotation JSON file.");
        return;
    }
    FILE *file = _wfopen(path, L"rb");
    if (!file) return;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return; }
    errno = 0;
    long length = ftell(file);
    if (length <= 0 || length > 16 * 1024 * 1024 || errno != 0 ||
        fseek(file, 0, SEEK_SET) != 0) { fclose(file); return; }
    char *text = (char *)malloc((size_t)length + 1);
    if (!text) { fclose(file); return; }
    size_t got = fread(text, 1, (size_t)length, file);
    fclose(file);
    text[got] = 0;
    int count = MAX_ANNOTATIONS;
    if (golden_document_parse_utf8(text, got, g.annotations, &count)) g.annotation_count = count;
    else show_error(L"The annotation JSON is invalid and was not loaded.");
    free(text);
}

static BOOL save_annotations(void) {
    if (!g.image_path[0]) return FALSE;
    wchar_t path[MAX_PATH * 4];
    if (!json_path_for(g.image_path, path, _countof(path))) {
        show_error(L"The resource path is too long to save its annotation JSON file.");
        return FALSE;
    }
    size_t json_length = 0;
    char *json = golden_document_serialize_utf8(g.annotations, g.annotation_count, &json_length);
    if (!json) { show_error(L"Could not serialize the annotations."); return FALSE; }
    BOOL ok = golden_atomic_write_bytes(path, json, json_length);
    free(json);
    if (ok) g.dirty = FALSE;
    else show_error(L"Could not finish writing the annotation JSON file.");
    InvalidateRect(g.editor, NULL, FALSE);
    return ok;
}

static BOOL maybe_save(void) {
    if (!g.dirty) return TRUE;
    int answer = MessageBoxW(g.main, L"Save annotation changes?", APP_NAME,
                             MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) return FALSE;
    if (answer == IDYES) return save_annotations();
    return TRUE;
}

static void free_tree_item(HWND tree, HTREEITEM item) {
    while (item) {
        TVITEMW info = {0};
        info.mask = TVIF_PARAM;
        info.hItem = item;
        TreeView_GetItem(tree, &info);
        if (info.lParam) {
            ResourceTreeNode *node = (ResourceTreeNode *)info.lParam;
            free(node->path);
            free(node);
        }
        HTREEITEM child = TreeView_GetChild(tree, item);
        if (child) free_tree_item(tree, child);
        item = TreeView_GetNextSibling(tree, item);
    }
}

static HTREEITEM insert_path_item(HWND tree, HTREEITEM parent, const wchar_t *label,
                                  const wchar_t *path, BOOL directory) {
    ResourceTreeNode *node = (ResourceTreeNode *)calloc(1, sizeof(*node));
    if (!node) return NULL;
    node->kind = directory ? RESOURCE_DIRECTORY : RESOURCE_PNG;
    node->path = dup_wide(path);
    if (!node->path) { free(node); return NULL; }
    TVINSERTSTRUCTW insert = {0};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = (wchar_t *)label;
    insert.item.lParam = (LPARAM)node;
    HTREEITEM item = TreeView_InsertItem(tree, &insert);
    if (!item) { free(node->path); free(node); }
    return item;
}

static ResourceTreeNode *tree_node_data(HTREEITEM item) {
    if (!item) return NULL;
    TVITEMW info = {0};
    info.mask = TVIF_PARAM;
    info.hItem = item;
    return TreeView_GetItem(g.tree, &info) ? (ResourceTreeNode *)info.lParam : NULL;
}

static HTREEITEM find_resource_item(HTREEITEM item, const wchar_t *path) {
    while (item) {
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->path && !_wcsicmp(node->path, path)) return item;
        HTREEITEM found = find_resource_item(TreeView_GetChild(g.tree, item), path);
        if (found) return found;
        item = TreeView_GetNextSibling(g.tree, item);
    }
    return NULL;
}

static HTREEITEM find_annotation_item(int annotation_index) {
    if (!g.tree || !g.image_path[0]) return NULL;
    HTREEITEM resource = find_resource_item(TreeView_GetRoot(g.tree), g.image_path);
    if (!resource) return NULL;
    for (HTREEITEM item = TreeView_GetChild(g.tree, resource); item;
         item = TreeView_GetNextSibling(g.tree, item)) {
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->kind == RESOURCE_ANNOTATION &&
            node->annotation_index == annotation_index) return item;
    }
    return NULL;
}

static void sync_tree_annotation_selection(void) {
    if (!g.tree || !g.image_path[0]) return;
    HTREEITEM target = g.selected >= 0 ? find_annotation_item(g.selected) :
        find_resource_item(TreeView_GetRoot(g.tree), g.image_path);
    if (!target) return;
    BOOL was_rebuilding = g.rebuilding_resources;
    g.rebuilding_resources = TRUE;
    TreeView_EnsureVisible(g.tree, target);
    TreeView_SelectItem(g.tree, target);
    g.rebuilding_resources = was_rebuilding;
}

static void delete_annotation_nodes(HTREEITEM item) {
    while (item) {
        HTREEITEM next = TreeView_GetNextSibling(g.tree, item);
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->kind == RESOURCE_ANNOTATION) {
            free(node->path);
            free(node);
            TreeView_DeleteItem(g.tree, item);
        } else {
            delete_annotation_nodes(TreeView_GetChild(g.tree, item));
        }
        item = next;
    }
}

static void refresh_annotation_tree(void) {
    if (!g.tree) return;
    g.rebuilding_resources = TRUE;
    delete_annotation_nodes(TreeView_GetRoot(g.tree));
    if (g.image_path[0]) {
        HTREEITEM resource = find_resource_item(TreeView_GetRoot(g.tree), g.image_path);
        HTREEITEM selected_annotation = NULL;
        for (int i = 0; resource && i < g.annotation_count; ++i) {
            ResourceTreeNode *node = (ResourceTreeNode *)calloc(1, sizeof(*node));
            if (!node) break;
            node->kind = RESOURCE_ANNOTATION;
            node->annotation_index = i;
            TVINSERTSTRUCTW insert = {0};
            insert.hParent = resource;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            insert.item.pszText = g.annotations[i].name;
            insert.item.lParam = (LPARAM)node;
            HTREEITEM child = TreeView_InsertItem(g.tree, &insert);
            if (!child) free(node);
            else if (i == g.selected) selected_annotation = child;
        }
        if (resource) TreeView_Expand(g.tree, resource, TVE_EXPAND);
        if (selected_annotation) TreeView_SelectItem(g.tree, selected_annotation);
    }
    g.rebuilding_resources = FALSE;
}

typedef struct {
    wchar_t *name;
    wchar_t *path;
    BOOL directory;
} ResourceEntry;

static int compare_resources(const void *left, const void *right) {
    const ResourceEntry *a = (const ResourceEntry *)left;
    const ResourceEntry *b = (const ResourceEntry *)right;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    return _wcsicmp(a->name, b->name);
}

static void populate_directory(HTREEITEM parent, const wchar_t *directory) {
    wchar_t pattern[MAX_PATH * 4];
    if (!golden_path_join(directory, L"*", pattern, _countof(pattern))) return;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    ResourceEntry *entries = NULL;
    size_t count = 0, capacity = 0;
    do {
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        BOOL directory_entry = !!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (directory_entry && (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN |
            FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        if (!directory_entry && !ends_with_png(data.cFileName)) continue;
        wchar_t full[MAX_PATH * 4];
        if (!golden_path_join(directory, data.cFileName,
                              full, _countof(full))) continue;
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2) break;
            size_t next = capacity ? capacity * 2 : 16;
            if (next > SIZE_MAX / sizeof(*entries)) break;
            ResourceEntry *grown = (ResourceEntry *)realloc(entries, next * sizeof(*entries));
            if (!grown) break;
            entries = grown;
            capacity = next;
        }
        entries[count].name = dup_wide(data.cFileName);
        entries[count].path = dup_wide(full);
        entries[count].directory = directory_entry;
        if (entries[count].name && entries[count].path) count++;
        else { free(entries[count].name); free(entries[count].path); }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    qsort(entries, count, sizeof(*entries), compare_resources);
    for (size_t i = 0; i < count; ++i) {
        HTREEITEM node = insert_path_item(g.tree, parent, entries[i].name,
                                          entries[i].path, entries[i].directory);
        if (node && entries[i].directory) populate_directory(node, entries[i].path);
        free(entries[i].name);
        free(entries[i].path);
    }
    free(entries);
}

static void refresh_resources(void) {
    g.rebuilding_resources = TRUE;
    free_tree_item(g.tree, TreeView_GetRoot(g.tree));
    TreeView_DeleteAllItems(g.tree);
    if (g.root[0]) {
        const wchar_t *label = PathFindFileNameW(g.root);
        if (!*label) label = g.root;
        HTREEITEM root = insert_path_item(g.tree, TVI_ROOT, label, g.root, TRUE);
        populate_directory(root, g.root);
        TreeView_Expand(g.tree, root, TVE_EXPAND);
    }
    g.rebuilding_resources = FALSE;
    refresh_annotation_tree();
    update_capture_availability();
}

static void remember_root(void) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Goldens", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"LastFolder", 0, REG_SZ, (BYTE *)g.root,
                       (DWORD)((wcslen(g.root) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void load_remembered_root(void) {
    HKEY key;
    DWORD type, bytes = sizeof(g.root);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Goldens", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"LastFolder", NULL, &type, (BYTE *)g.root, &bytes) != ERROR_SUCCESS ||
            type != REG_SZ || bytes < sizeof(wchar_t) || bytes > sizeof(g.root) ||
            bytes % sizeof(wchar_t) || g.root[bytes / sizeof(wchar_t) - 1] != 0 ||
            GetFileAttributesW(g.root) == INVALID_FILE_ATTRIBUTES) g.root[0] = 0;
        RegCloseKey(key);
    }
}

static void initialize_startup_root(void) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        DWORD attrs = GetFileAttributesW(argv[1]);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            DWORD length = GetFullPathNameW(argv[1], _countof(g.root), g.root, NULL);
            if (!length || length >= _countof(g.root)) g.root[0] = 0;
        }
    }
    if (argv) LocalFree(argv);
    if (!g.root[0]) {
        DWORD length = GetCurrentDirectoryW(_countof(g.root), g.root);
        if (!length || length >= _countof(g.root)) g.root[0] = 0;
    }
    DWORD attrs = GetFileAttributesW(g.root);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        g.root[0] = 0;
        load_remembered_root();
    }
    if (g.root[0]) {
        if (!golden_path_copy(g.root, g.current_dir,
                              _countof(g.current_dir))) g.root[0] = 0;
    }
}

static int CALLBACK browse_callback(HWND hwnd, UINT msg, LPARAM lp, LPARAM data) {
    if (msg == BFFM_INITIALIZED && g.root[0])
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, (LPARAM)g.root);
    return 0;
}

static void open_folder(void) {
    if (!maybe_save()) return;
    BROWSEINFOW bi = {0};
    bi.hwndOwner = g.main;
    bi.lpszTitle = L"Choose a golden resources folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_callback;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t path[MAX_PATH * 4];
    if (SHGetPathFromIDListW(id, path)) {
        if (!golden_path_copy(path, g.root, _countof(g.root)) ||
            !golden_path_copy(path, g.current_dir,
                              _countof(g.current_dir))) {
            show_error(L"The selected folder path is too long.");
            CoTaskMemFree(id);
            return;
        }
        clear_image();
        clear_preview();
        g.image_path[0] = 0;
        g.annotation_count = 0;
        g.selected = -1;
        update_tool_availability();
        g.dirty = FALSE;
        g.undo_count = g.redo_count = 0;
        remember_root();
        refresh_resources();
        update_status();
        SetWindowTextW(g.main, APP_NAME);
        update_context_label();
        InvalidateRect(g.editor, NULL, FALSE);
    }
    CoTaskMemFree(id);
}

static BOOL load_resource(const wchar_t *path) {
    wchar_t next_path[MAX_PATH * 4], next_directory[MAX_PATH * 4];
    if (!golden_path_copy(path, next_path, _countof(next_path)) ||
        !parent_dir_for(path, next_directory, _countof(next_directory))) {
        show_error(L"The selected resource path is too long.");
        return FALSE;
    }
    if (!maybe_save()) return FALSE;
    if (!load_png(next_path)) { show_error(L"Could not decode the selected PNG file."); return FALSE; }
    clear_preview();
    g.resource_visible = TRUE;
    g.zoom = 0.0;
    g.pan_x = g.pan_y = 0;
    golden_path_copy(next_path, g.image_path, _countof(g.image_path));
    golden_path_copy(next_directory, g.current_dir, _countof(g.current_dir));
    update_status();
    load_annotations(next_path);
    update_tool_availability();
    refresh_annotation_tree();
    update_capture_availability();
    update_context_label();
    InvalidateRect(g.editor, NULL, FALSE);
    wchar_t title[MAX_PATH * 4 + 32];
    _snwprintf(title, _countof(title), L"Goldens — %s",
               PathFindFileNameW(next_path));
    SetWindowTextW(g.main, title);
    return TRUE;
}

static void image_layout(HWND hwnd, RECT *dest, double *scale) {
    RECT client;
    GetClientRect(hwnd, &client);
    UINT width = active_width(), height = active_height();
    if ((!g.preview_mode && !active_pixels()) || !width || !height) {
        SetRectEmpty(dest); *scale = 1.0; return;
    }
    GoldenViewport viewport = golden_compute_viewport(
        (int)width, (int)height, client.right, client.bottom,
        30, g.zoom, g.pan_x, g.pan_y);
    *dest = viewport.destination;
    *scale = viewport.scale;
}

static BOOL client_to_image(HWND hwnd, POINT client, POINT *image) {
    RECT dest;
    double scale;
    image_layout(hwnd, &dest, &scale);
    GoldenViewport viewport = {dest, scale};
    return golden_view_to_image(&viewport, client, (int)active_width(),
                                (int)active_height(), image);
}

static RECT annotation_screen_rect_for_layout(const RECT *dest, double scale,
                                               const RECT *boundary) {
    RECT r = {
        dest->left + (LONG)(boundary->left * scale),
        dest->top + (LONG)(boundary->top * scale),
        dest->left + (LONG)(boundary->right * scale),
        dest->top + (LONG)(boundary->bottom * scale)
    };
    return r;
}

static RECT annotation_screen_rect(HWND hwnd, const RECT *boundary) {
    RECT destination;
    double scale;
    image_layout(hwnd, &destination, &scale);
    return annotation_screen_rect_for_layout(&destination, scale, boundary);
}

static void hide_annotation_tooltip(HWND hwnd) {
    KillTimer(hwnd, EDITOR_TOOLTIP_TIMER);
    g.tooltip_pending = -1;
    if (g.tooltip_visible >= 0)
        golden_tooltip_hide(g.editor_tooltip, &g.editor_tooltip_tool);
    g.tooltip_visible = -1;
}

static void update_annotation_hover(HWND hwnd, POINT client) {
    if (!g.editor_mouse_tracking) {
        TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, hwnd, 0};
        g.editor_mouse_tracking = TrackMouseEvent(&tracking);
    }
    POINT image;
    int hit = -1;
    if (!g.preview_mode && !g.panning && !g.drag_mode && !g.drawing &&
        client_to_image(hwnd, client, &image))
        hit = golden_hit_annotation(g.annotations, g.annotation_count, image);
    GoldenTooltipHoverAction action = golden_tooltip_hover_action(
        hit, g.tooltip_pending, g.tooltip_visible);
    if (action == GOLDEN_TOOLTIP_HOVER_NONE) return;
    hide_annotation_tooltip(hwnd);
    if (action == GOLDEN_TOOLTIP_HOVER_SCHEDULE) {
        g.tooltip_pending = hit;
        SetTimer(hwnd, EDITOR_TOOLTIP_TIMER, EDITOR_TOOLTIP_DELAY_MS, NULL);
    }
}

static void show_pending_annotation_tooltip(HWND hwnd) {
    KillTimer(hwnd, EDITOR_TOOLTIP_TIMER);
    if (!g.editor_tooltip || g.tooltip_pending < 0 ||
        g.tooltip_pending >= g.annotation_count) return;
    POINT cursor;
    GetCursorPos(&cursor);
    POINT client = cursor;
    ScreenToClient(hwnd, &client);
    POINT image;
    int hit = client_to_image(hwnd, client, &image) ?
        golden_hit_annotation(g.annotations, g.annotation_count, image) : -1;
    if (hit != g.tooltip_pending || g.preview_mode || g.panning ||
        g.drag_mode || g.drawing) {
        g.tooltip_pending = -1;
        return;
    }
    wcsncpy(g.tooltip_text, g.annotations[hit].name, _countof(g.tooltip_text) - 1);
    g.tooltip_text[_countof(g.tooltip_text) - 1] = 0;
    POINT position = {cursor.x + 12, cursor.y + 20};
    golden_tooltip_show(g.editor_tooltip, &g.editor_tooltip_tool,
                        g.tooltip_text, position);
    g.tooltip_visible = hit;
    g.tooltip_pending = -1;
}

static void draw_editor(HWND hwnd, HDC dc) {
    RECT client;
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, (HBRUSH)(COLOR_APPWORKSPACE + 1));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(230, 230, 230));
    BYTE *pixels = active_pixels();
    UINT image_w = active_width(), image_h = active_height();
    if (!pixels) {
        const wchar_t *message;
        if (g.preview_mode)
            message = g.preview_loading ? L"Capturing window preview…" : L"Preview unavailable for this window";
        else
            message = g.root[0] ? L"Select a PNG from the resource tree" : L"Open a resource folder to begin";
        DrawTextW(dc, message, -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    RECT dest;
    double scale;
    image_layout(hwnd, &dest, &scale);
    if (!golden_draw_cached_bgra_image(&g.image_cache, dc, pixels,
                                       image_w, image_h, &dest, scale,
                                       g.image_revision))
        golden_draw_bgra_image(dc, pixels, image_w, image_h, &dest, scale);
    FrameRect(dc, &dest, (HBRUSH)GetStockObject(BLACK_BRUSH));
    for (int i = 0; !g.preview_mode && i < g.annotation_count; ++i) {
        RECT r = annotation_screen_rect_for_layout(
            &dest, scale, &g.annotations[i].boundary);
        golden_draw_boundary(dc, &r,
            i == g.selected ? RGB(255, 180, 0) : RGB(0, 220, 255),
            i == g.selected ? 3 : 2, PS_SOLID);
        if (i == g.selected) {
            RECT handle = {r.right - 5, r.bottom - 5, r.right + 5, r.bottom + 5};
            FillRect(dc, &handle, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }
        if (g.annotations[i].has_click) {
            int cx = r.left + (int)((r.right - r.left) * g.annotations[i].click_x);
            int cy = r.top + (int)((r.bottom - r.top) * g.annotations[i].click_y);
            MoveToEx(dc, cx - 6, cy, NULL); LineTo(dc, cx + 7, cy);
            MoveToEx(dc, cx, cy - 6, NULL); LineTo(dc, cx, cy + 7);
        }
    }
    if (!g.preview_mode && g.drawing) {
        RECT boundary = {min(g.draw_start.x, g.draw_current.x), min(g.draw_start.y, g.draw_current.y),
                         max(g.draw_start.x, g.draw_current.x), max(g.draw_start.y, g.draw_current.y)};
        RECT r = annotation_screen_rect_for_layout(&dest, scale, &boundary);
        golden_fill_tinted_rect(dc, &r, RGB(255, 150, 0), 112);
        golden_draw_boundary(dc, &r, RGB(255, 210, 0), 3, PS_SOLID);
    }
    wchar_t info_text[512];
    if (g.preview_mode)
        _snwprintf(info_text, _countof(info_text), L"Window preview: %s  •  %u × %u px  •  %.0f%%",
                   g.preview_title, image_w, image_h, scale * 100.0);
    else
        _snwprintf(info_text, _countof(info_text), L"%u × %u px  •  %d annotation%s%s  •  %.0f%%",
            image_w, image_h, g.annotation_count, g.annotation_count == 1 ? L"" : L"s",
            g.dirty ? L"  •  Unsaved" : L"", scale * 100.0);
    SetTextColor(dc, RGB(230, 230, 230));
    TextOutW(dc, 10, 7, info_text, (int)wcslen(info_text));
}

static void paint_editor(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC paint_dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    int width = max(1, client.right - client.left);
    int height = max(1, client.bottom - client.top);
    BOOL buffered = golden_back_buffer_ensure(&g.editor_buffer, paint_dc,
                                              width, height);
    HDC target = buffered ? g.editor_buffer.dc : paint_dc;
    draw_editor(hwnd, target);
    if (buffered)
        BitBlt(paint_dc, 0, 0, width, height, g.editor_buffer.dc, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

static void rename_selected(void) {
    if (g.selected < 0) return;
    wchar_t name[128];
    wcscpy(name, g.annotations[g.selected].name);
    if (!prompt_annotation_name(name, _countof(name), g.selected)) return;
    push_undo();
    wcscpy(g.annotations[g.selected].name, name);
    g.dirty = TRUE;
    refresh_annotation_tree();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void delete_selected(void) {
    if (g.selected < 0) return;
    push_undo();
    memmove(&g.annotations[g.selected], &g.annotations[g.selected + 1],
            sizeof(Annotation) *
                (size_t)(g.annotation_count - g.selected - 1));
    g.annotation_count--;
    if (g.selected >= g.annotation_count) g.selected = g.annotation_count - 1;
    g.dirty = TRUE;
    update_tool_availability();
    refresh_annotation_tree();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void clear_click(void) {
    if (g.selected < 0 || !g.annotations[g.selected].has_click) return;
    push_undo();
    g.annotations[g.selected].has_click = FALSE;
    g.dirty = TRUE;
    InvalidateRect(g.editor, NULL, FALSE);
}

static void deselect_annotation(void) {
    if (g.selected < 0) return;
    g.selected = -1;
    update_tool_availability();
    sync_tree_annotation_selection();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void set_editor_cursor(void) {
    LPCWSTR cursor = g.drag_mode == 2 ? IDC_SIZENWSE :
                     g.panning || g.drag_mode == 1 ? IDC_SIZEALL :
                     g.preview_mode || g.tool == TOOL_SELECT ? IDC_ARROW :
                     IDC_CROSS;
    SetCursor(LoadCursorW(NULL, cursor));
}

static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint_editor(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            set_editor_cursor();
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        hide_annotation_tooltip(hwnd);
        SetFocus(hwnd);
        if (g.preview_mode) {
            g.panning = TRUE;
            g.pan_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            g.pan_origin_x = g.pan_x; g.pan_origin_y = g.pan_y;
            SetCapture(hwnd);
            set_editor_cursor();
            return 0;
        }
        POINT client = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, image;
        if (!client_to_image(hwnd, client, &image)) {
            if (g.tool == TOOL_SELECT) deselect_annotation();
            return 0;
        }
        if (g.tool == TOOL_CLICK) {
            if (g.selected >= 0 && g.selected < g.annotation_count &&
                PtInRect(&g.annotations[g.selected].boundary, image)) {
                push_undo();
                golden_set_click(&g.annotations[g.selected], image);
                g.dirty = TRUE;
            } else MessageBeep(MB_ICONWARNING);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g.tool == TOOL_RECTANGLE) {
            if (g.annotation_count >= MAX_ANNOTATIONS) {
                show_error(L"The annotation limit has been reached.");
                return 0;
            }
            g.selected = -1;
            update_tool_availability();
            sync_tree_annotation_selection();
            g.drawing = TRUE;
            g.draw_start = g.draw_current = image;
            SetCapture(hwnd);
        } else {
            int hit = golden_hit_annotation(g.annotations, g.annotation_count, image);
            if (hit >= 0) {
                g.selected = hit;
                update_tool_availability();
                sync_tree_annotation_selection();
                RECT screen = annotation_screen_rect(hwnd, &g.annotations[hit].boundary);
                g.drag_mode = abs(client.x - screen.right) <= 9 && abs(client.y - screen.bottom) <= 9 ? 2 : 1;
                g.drag_start = image;
                g.drag_original = g.annotations[hit].boundary;
                push_undo();
                SetCapture(hwnd);
                set_editor_cursor();
            } else {
                deselect_annotation();
                g.panning = TRUE;
                g.pan_start = client;
                g.pan_origin_x = g.pan_x;
                g.pan_origin_y = g.pan_y;
                SetCapture(hwnd);
                set_editor_cursor();
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        update_annotation_hover(hwnd,
            (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (g.panning) {
            g.pan_x = g.pan_origin_x + GET_X_LPARAM(lp) - g.pan_start.x;
            g.pan_y = g.pan_origin_y + GET_Y_LPARAM(lp) - g.pan_start.y;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g.drag_mode || g.drawing) {
            POINT client = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, image;
            RECT dest; double scale; image_layout(hwnd, &dest, &scale);
            image.x = (LONG)((client.x - dest.left) / scale);
            image.y = (LONG)((client.y - dest.top) / scale);
            image.x = min((LONG)g.image_w, max(0, image.x));
            image.y = min((LONG)g.image_h, max(0, image.y));
            if (g.drawing) g.draw_current = image;
            else if (g.selected >= 0) {
                RECT *r = &g.annotations[g.selected].boundary;
                if (g.drag_mode == 1) {
                    int dx = image.x - g.drag_start.x, dy = image.y - g.drag_start.y;
                    int width = g.drag_original.right - g.drag_original.left;
                    int height = g.drag_original.bottom - g.drag_original.top;
                    r->left = min((LONG)g.image_w - width, max(0, g.drag_original.left + dx));
                    r->top = min((LONG)g.image_h - height, max(0, g.drag_original.top + dy));
                    r->right = r->left + width; r->bottom = r->top + height;
                } else {
                    r->right = min((LONG)g.image_w, max(r->left + 1, image.x));
                    r->bottom = min((LONG)g.image_h, max(r->top + 1, image.y));
                }
                g.dirty = TRUE;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        g.editor_mouse_tracking = FALSE;
        hide_annotation_tooltip(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == EDITOR_TOOLTIP_TIMER) {
            show_pending_annotation_tooltip(hwnd);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g.panning) {
            g.panning = FALSE;
            ReleaseCapture();
            set_editor_cursor();
            if (g.preview_mode) request_preview_frame();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g.drag_mode) {
            g.drag_mode = 0;
            ReleaseCapture();
            set_editor_cursor();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g.drawing) {
            ReleaseCapture();
            g.drawing = FALSE;
            RECT r = golden_normalize_rect(g.draw_start, g.draw_current);
            if (r.right - r.left >= 2 && r.bottom - r.top >= 2) {
                push_undo();
                Annotation *a = &g.annotations[g.annotation_count];
                ZeroMemory(a, sizeof(*a));
                make_unique_name(a->name, _countof(a->name));
                a->boundary = r;
                g.selected = g.annotation_count++;
                g.dirty = TRUE;
                refresh_annotation_tree();
                HTREEITEM item = find_annotation_item(g.selected);
                if (item) PostMessageW(g.main, WM_BEGIN_TREE_RENAME, 0, (LPARAM)item);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MBUTTONDOWN:
        g.panning = TRUE;
        g.pan_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g.pan_origin_x = g.pan_x; g.pan_origin_y = g.pan_y;
        SetCapture(hwnd);
        set_editor_cursor();
        return 0;
    case WM_MBUTTONUP:
        if (g.panning) {
            g.panning = FALSE;
            ReleaseCapture();
            set_editor_cursor();
            if (g.preview_mode) request_preview_frame();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT anchor = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &anchor);
        zoom_by(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.25 : 0.8, &anchor);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (!g.preview_mode && g.tool == TOOL_SELECT) rename_selected();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_DELETE) delete_selected();
        else if (wp == VK_F2) rename_selected();
        else if (wp == 'Z' && GetKeyState(VK_CONTROL) < 0) undo_action();
        else if (wp == 'Y' && GetKeyState(VK_CONTROL) < 0) redo_action();
        else if (wp == '0') { g.zoom = 0.0; g.pan_x = g.pan_y = 0; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == '1') { g.zoom = 1.0; g.pan_x = g.pan_y = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_CAPTURECHANGED:
        g.panning = FALSE;
        g.drag_mode = 0;
        g.drawing = FALSE;
        set_editor_cursor();
        return 0;
    case WM_NCDESTROY:
        golden_image_cache_release(&g.image_cache);
        golden_back_buffer_release(&g.editor_buffer);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void refresh_windows(void) {
    GoldenWindowInfo next[MAX_WINDOWS];
    int next_count = golden_collect_windows(g.main, next, MAX_WINDOWS);
    if (golden_window_lists_equal(g.window_items, g.window_count, next, next_count)) return;
    HWND selected_window = selected_capture_window();
    g.rebuilding_windows = TRUE;
    TreeView_DeleteAllItems(g.windows);
    memcpy(g.window_items, next,
           sizeof(GoldenWindowInfo) * (size_t)next_count);
    g.window_count = next_count;
    HTREEITEM group = NULL, selected_item = NULL;
    wchar_t previous[128] = L"";
    for (int i = 0; i < g.window_count; ++i) {
        if (_wcsicmp(previous, g.window_items[i].app)) {
            if (group) TreeView_Expand(g.windows, group, TVE_EXPAND);
            wcscpy(previous, g.window_items[i].app);
            TVINSERTSTRUCTW insert = {0};
            insert.hParent = TVI_ROOT; insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT;
            insert.item.pszText = g.window_items[i].app;
            group = TreeView_InsertItem(g.windows, &insert);
        }
        TVINSERTSTRUCTW insert = {0};
        insert.hParent = group; insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = g.window_items[i].title;
        insert.item.lParam = (LPARAM)g.window_items[i].id;
        HTREEITEM leaf = TreeView_InsertItem(g.windows, &insert);
        if ((HWND)g.window_items[i].id == selected_window) selected_item = leaf;
    }
    if (group) TreeView_Expand(g.windows, group, TVE_EXPAND);
    if (selected_item) TreeView_SelectItem(g.windows, selected_item);
    g.rebuilding_windows = FALSE;
    if (g.preview_target) {
        BOOL found = FALSE;
        for (int i = 0; i < g.window_count; ++i)
            if ((HWND)g.window_items[i].id == g.preview_target) { found = TRUE; break; }
        if (!found) { clear_preview(); InvalidateRect(g.editor, NULL, FALSE); }
    }
    update_capture_availability();
}

static HWND selected_capture_window(void) {
    if (!g.windows) return NULL;
    HTREEITEM selected = TreeView_GetSelection(g.windows);
    if (!selected) return NULL;
    TVITEMW item = {0};
    item.mask = TVIF_PARAM; item.hItem = selected;
    if (!TreeView_GetItem(g.windows, &item) || !item.lParam) return NULL;
    HWND target = (HWND)item.lParam;
    return IsWindow(target) ? target : NULL;
}

static HWND clicked_capture_window(void) {
    DWORD position = GetMessagePos();
    TVHITTESTINFO hit = {0};
    hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(g.windows, &hit.pt);
    TreeView_HitTest(g.windows, &hit);
    if (!hit.hItem || !(hit.flags & TVHT_ONITEM)) return NULL;
    TVITEMW item = {0};
    item.mask = TVIF_PARAM;
    item.hItem = hit.hItem;
    if (!TreeView_GetItem(g.windows, &item)) return NULL;
    HWND target = (HWND)item.lParam;
    return target && IsWindow(target) ? target : NULL;
}

static ResourceTreeNode *clicked_resource_node(void) {
    DWORD position = GetMessagePos();
    TVHITTESTINFO hit = {0};
    hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(g.tree, &hit.pt);
    TreeView_HitTest(g.tree, &hit);
    if (!hit.hItem || !(hit.flags & TVHT_ONITEM)) return NULL;
    return tree_node_data(hit.hItem);
}

static ResourceTreeNode *selected_active_resource_node(void) {
    if (!g.tree || !g.image_path[0]) return NULL;
    ResourceTreeNode *node = tree_node_data(TreeView_GetSelection(g.tree));
    if (!node) return NULL;
    if (node->kind == RESOURCE_PNG)
        return node->path && !_wcsicmp(node->path, g.image_path) ? node : NULL;
    if (node->kind == RESOURCE_ANNOTATION && node->annotation_index >= 0 &&
        node->annotation_index < g.annotation_count) return node;
    return NULL;
}

static void update_capture_availability(void) {
    BOOL window_selected = selected_capture_window() != NULL;
    BOOL resource_selected = selected_active_resource_node() != NULL;
    if (g.window_buttons[0]) EnableWindow(g.window_buttons[0], window_selected);
    if (g.window_buttons[1])
        EnableWindow(g.window_buttons[1], window_selected && resource_selected);
    if (g.capture_menu) {
        EnableMenuItem(g.capture_menu, ID_CAPTURE, MF_BYCOMMAND |
            (window_selected ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(g.capture_menu, ID_RECAPTURE, MF_BYCOMMAND |
            (window_selected && resource_selected ? MF_ENABLED : MF_GRAYED));
    }
}

static void clear_tree_selection_on_blank_click(HWND tree) {
    DWORD position = GetMessagePos();
    TVHITTESTINFO hit = {0};
    hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(tree, &hit.pt);
    TreeView_HitTest(tree, &hit);
    if (hit.flags & (TVHT_ONITEM | TVHT_ONITEMBUTTON)) return;
    if (tree == g.tree) {
        HTREEITEM selected = TreeView_GetSelection(tree);
        ResourceTreeNode *node = tree_node_data(selected);
        if (node && node->kind == RESOURCE_ANNOTATION) {
            HTREEITEM resource = TreeView_GetParent(tree, selected);
            if (resource) {
                TreeView_SelectItem(tree, resource);
                return;
            }
        }
    }
    TreeView_SelectItem(tree, NULL);
}

static BOOL capture_preview_frame(HWND target, GoldenPreviewSurface *surface,
                                  void *context) {
    UNREFERENCED_PARAMETER(context);
    return golden_preview_surface_capture(surface, target);
}

static void request_preview_frame(void) {
    if (!g.preview_mode || !g.preview_target || g.preview_loading ||
        g.panning || IsIconic(g.main)) return;
    if (!IsWindow(g.preview_target)) {
        clear_preview();
        InvalidateRect(g.editor, NULL, FALSE);
        return;
    }
    g.preview_loading = golden_preview_service_request(
        &g.preview_service, g.preview_target, g.preview_generation);
}

static void preview_window(HWND target) {
    if (!target || !IsWindow(target) || target == g.main) return;
    if (target == g.preview_target && g.preview_mode) {
        request_preview_frame();
        return;
    }
    clear_preview();
    g.preview_target = target;
    GetWindowTextW(target, g.preview_title, _countof(g.preview_title));
    g.preview_mode = TRUE;
    update_tool_availability();
    g.zoom = 0.0;
    g.pan_x = g.pan_y = 0;
    update_context_label();
    request_preview_frame();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void refresh_preview_metadata(void) {
    if (!g.preview_target) return;
    if (!IsWindow(g.preview_target)) {
        clear_preview();
        InvalidateRect(g.editor, NULL, FALSE);
        return;
    }
    wchar_t title[256] = L"";
    GetWindowTextW(g.preview_target, title, _countof(title));
    if (wcscmp(title, g.preview_title)) {
        wcscpy(g.preview_title, title);
        update_context_label();
        InvalidateRect(g.editor, NULL, FALSE);
    }
}

static BOOL capture_window_to(HWND target, const wchar_t *path) {
    if (!IsWindow(target)) { show_error(L"The selected window is no longer open."); return FALSE; }
    WINDOWPLACEMENT placement = {0};
    placement.length = sizeof(placement);
    GetWindowPlacement(target, &placement);
    BOOL was_minimized = IsIconic(target);
    ShowWindow(target, SW_RESTORE);
    ShowWindow(g.main, SW_MINIMIZE);
    SetForegroundWindow(target);
    Sleep(350);
    RECT rect;
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
        GetWindowRect(target, &rect);
    LONGLONG measured_width = (LONGLONG)rect.right - rect.left;
    LONGLONG measured_height = (LONGLONG)rect.bottom - rect.top;
    BOOL ok = FALSE;
    if (measured_width > 0 && measured_width <= INT_MAX / 4 &&
        measured_height > 0 && measured_height <= INT_MAX) {
        int width = (int)measured_width;
        int height = (int)measured_height;
        HDC screen = GetDC(NULL);
        HDC memory = CreateCompatibleDC(screen);
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        BYTE *bits = NULL;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        if (bitmap && memory && bits) {
            HGDIOBJ old = SelectObject(memory, bitmap);
            if (BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT)) {
                golden_bgra_force_opaque(bits, (UINT)width, (UINT)height,
                                         (UINT)width * 4u);
                ok = save_png_pixels(path, bits, (UINT)width, (UINT)height,
                                     (UINT)width * 4u);
            }
            SelectObject(memory, old);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(NULL, screen);
    }
    if (was_minimized) ShowWindow(target, SW_MINIMIZE);
    ShowWindow(g.main, SW_RESTORE);
    SetForegroundWindow(g.main);
    if (!ok) show_error(L"The selected window could not be captured.");
    return ok;
}

static BOOL valid_capture_name(const wchar_t *name) {
    if (!name[0] || !wcscmp(name, L".") || !wcscmp(name, L"..")) return FALSE;
    size_t length = wcslen(name);
    return name[length - 1] != L'.' && !wcspbrk(name, L"<>:\"/\\|?*");
}

static void normalize_capture_name(wchar_t *name) {
    wchar_t *start = name;
    while (*start && iswspace(*start)) ++start;
    if (start != name) memmove(name, start, (wcslen(start) + 1) * sizeof(wchar_t));
    size_t length = wcslen(name);
    while (length && iswspace(name[length - 1])) name[--length] = 0;
    if (ends_with_png(name)) name[length - 4] = 0;
}

static BOOL rename_resource_file(const wchar_t *old_path, const wchar_t *edited_name) {
    wchar_t name[256];
    wcsncpy(name, edited_name, _countof(name) - 1);
    name[_countof(name) - 1] = 0;
    normalize_capture_name(name);
    if (!valid_capture_name(name)) {
        show_error(L"Enter a valid Windows resource name.");
        return FALSE;
    }
    wchar_t directory[MAX_PATH * 4], new_path[MAX_PATH * 4];
    if (!parent_dir_for(old_path, directory, _countof(directory)) ||
        !golden_path_join_extension(directory, name, L".png",
                                    new_path, _countof(new_path))) {
        show_error(L"The renamed resource path is too long.");
        return FALSE;
    }
    if (!wcscmp(old_path, new_path)) return TRUE;

    GoldenResourceRenameResult result = golden_rename_resource_pair(old_path, new_path);
    if (result != GOLDEN_RENAME_OK) {
        const wchar_t *message = result == GOLDEN_RENAME_PNG_EXISTS ?
            L"A PNG with that resource name already exists in this directory." :
            result == GOLDEN_RENAME_JSON_EXISTS ?
            L"A JSON annotation file with that resource name already exists." :
            result == GOLDEN_RENAME_INVALID_PATH ?
            L"The PNG or JSON resource path is invalid or too long." :
            result == GOLDEN_RENAME_JSON_FAILED_ROLLED_BACK ?
            L"The JSON file could not be renamed, so the PNG rename was rolled back." :
            result == GOLDEN_RENAME_ROLLBACK_FAILED ?
            L"The JSON rename and PNG rollback both failed. The PNG and JSON may now have different names." :
            L"Windows could not rename the resource pair.";
        show_error(message);
        return FALSE;
    }
    if (!_wcsicmp(g.image_path, old_path)) {
        golden_path_copy(new_path, g.image_path, _countof(g.image_path));
        update_context_label();
        wchar_t title[MAX_PATH * 4 + 32];
        _snwprintf(title, _countof(title), L"Goldens — %s", PathFindFileNameW(new_path));
        SetWindowTextW(g.main, title);
    }
    golden_path_copy(new_path, g.pending_resource_selection,
                     _countof(g.pending_resource_selection));
    PostMessageW(g.main, WM_RESOURCE_RENAMED, 0, 0);
    return TRUE;
}

static BOOL begin_tree_rename(HTREEITEM item) {
    ResourceTreeNode *node = tree_node_data(item);
    if (!node || (node->kind != RESOURCE_PNG && node->kind != RESOURCE_ANNOTATION))
        return FALSE;
    if (node->kind == RESOURCE_ANNOTATION &&
        node->annotation_index >= 0 && node->annotation_index < g.annotation_count) {
        clear_preview();
        g.selected = node->annotation_index;
        update_tool_availability();
        InvalidateRect(g.editor, NULL, FALSE);
    }
    TreeView_SelectItem(g.tree, item);
    HWND edit = TreeView_EditLabel(g.tree, item);
    if (edit) {
        SendMessageW(edit, EM_LIMITTEXT,
            node->kind == RESOURCE_ANNOTATION ? 127 : 255, 0);
        SendMessageW(edit, EM_SETSEL, 0, -1);
    }
    return edit != NULL;
}

static BOOL ensure_capture_directory(void) {
    const wchar_t *candidate = g.current_dir[0] ? g.current_dir : g.root;
    DWORD attrs = GetFileAttributesW(candidate);
    if (candidate[0] && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return TRUE;
    wchar_t cwd[MAX_PATH * 4];
    DWORD length = GetCurrentDirectoryW(_countof(cwd), cwd);
    if (!length || length >= _countof(cwd)) return FALSE;
    attrs = GetFileAttributesW(cwd);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    if (!golden_path_copy(cwd, g.root, _countof(g.root)) ||
        !golden_path_copy(cwd, g.current_dir, _countof(g.current_dir)))
        return FALSE;
    refresh_resources();
    update_status();
    return TRUE;
}

static void capture_new(void) {
    HWND target = selected_capture_window();
    if (!target) { show_error(L"Select a window in the right column first."); return; }
    if (!ensure_capture_directory()) { show_error(L"The current directory is not available for captures."); return; }
    wchar_t name[128] = L"";
    if (!prompt_text(g.main, L"New screenshot", L"Resource name (without .png):", name, _countof(name))) return;
    normalize_capture_name(name);
    if (!valid_capture_name(name)) { show_error(L"Enter a valid Windows file name without path characters."); return; }
    const wchar_t *dir = g.current_dir[0] ? g.current_dir : g.root;
    wchar_t path[MAX_PATH * 4];
    if (!golden_path_join_extension(dir, name, L".png",
                                    path, _countof(path))) {
        show_error(L"The capture path is too long.");
        return;
    }
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        show_error(L"A PNG with that name already exists in this directory."); return;
    }
    if (!maybe_save()) return;
    if (capture_window_to(target, path)) {
        refresh_resources();
        load_resource(path);
    }
}

static void recapture_current(void) {
    HWND target = selected_capture_window();
    if (!target) { show_error(L"Select a window in the right column first."); return; }
    if (!selected_active_resource_node()) {
        show_error(L"Select an existing PNG resource to recapture.");
        return;
    }
    if (MessageBoxW(g.main, L"Replace the current PNG while preserving its annotations?",
                    APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    if (capture_window_to(target, g.image_path)) {
        clear_preview();
        if (load_png(g.image_path)) g.resource_visible = TRUE;
        g.zoom = 0.0;
        g.pan_x = g.pan_y = 0;
        InvalidateRect(g.editor, NULL, FALSE);
    }
}

static void toggle_splitter(HWND splitter) {
    if (splitter == g.left_splitter) g.left_collapsed = !g.left_collapsed;
    else if (splitter == g.right_splitter) g.right_collapsed = !g.right_collapsed;
    layout_children(g.main);
    InvalidateRect(g.left_splitter, NULL, TRUE);
    InvalidateRect(g.right_splitter, NULL, TRUE);
}

static LRESULT CALLBACK SplitterProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    BOOL left = hwnd == g.left_splitter;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
        BOOL collapsed = left ? g.left_collapsed : g.right_collapsed;
        RECT indicator = client;
        if (collapsed) {
            int thickness = max(3, client.right / 4);
            if (left) indicator.left = indicator.right - thickness;
            else indicator.right = indicator.left + thickness;
        } else {
            indicator.left = client.right / 2;
            indicator.right = indicator.left + 1;
        }
        FillRect(dc, &indicator, GetSysColorBrush(COLOR_3DSHADOW));
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
        return TRUE;
    case WM_LBUTTONDOWN: {
        BOOL collapsed = left ? g.left_collapsed : g.right_collapsed;
        g.splitter_dragging = TRUE;
        g.splitter_drag_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &g.splitter_drag_start);
        g.splitter_width_start = 0;
        if (!collapsed) {
            RECT pane;
            GetWindowRect(left ? g.tree : g.windows, &pane);
            g.splitter_width_start = MulDiv(pane.right - pane.left, 96,
                                            (int)GetDpiForWindow(g.main));
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g.splitter_dragging && GetCapture() == hwnd) {
            POINT current = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &current);
            int delta = MulDiv(current.x - g.splitter_drag_start.x, 96,
                               (int)GetDpiForWindow(g.main));
            int width = left ? g.splitter_width_start + delta :
                               g.splitter_width_start - delta;
            int minimum = left ? GOLDEN_RESOURCE_PANE_MIN : GOLDEN_WINDOWS_PANE_MIN;
            int maximum = left ? 520 : 560;
            BOOL *collapsed = left ? &g.left_collapsed : &g.right_collapsed;
            int *preferred = left ? &g.left_column_width : &g.right_column_width;
            BOOL next_collapsed = golden_pane_should_collapse(width, minimum, *collapsed);
            if (next_collapsed != *collapsed) {
                *collapsed = next_collapsed;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            if (!next_collapsed) *preferred = max(minimum, min(maximum, width));
            layout_children(g.main);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g.splitter_dragging && GetCapture() == hwnd) ReleaseCapture();
        g.splitter_dragging = FALSE;
        return 0;
    case WM_CAPTURECHANGED:
        g.splitter_dragging = FALSE;
        return 0;
    case WM_LBUTTONDBLCLK:
        if (GetCapture() == hwnd) ReleaseCapture();
        g.splitter_dragging = FALSE;
        toggle_splitter(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

static LRESULT CALLBACK ToolButtonProc(HWND hwnd, UINT message, WPARAM wp,
                                       LPARAM lp, UINT_PTR subclass_id,
                                       DWORD_PTR reference) {
    int index = (int)reference;
    switch (message) {
    case WM_MOUSEMOVE:
        if (IsWindowEnabled(hwnd) && g.hovered_tool != index) {
            int previous = g.hovered_tool;
            g.hovered_tool = index;
            if (previous >= 0 && previous < GOLDEN_TOOL_BUTTON_COUNT &&
                g.tool_buttons[previous])
                InvalidateRect(g.tool_buttons[previous], NULL, FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (g.hovered_tool == index) {
            g.hovered_tool = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_ENABLE:
        if (!wp && g.hovered_tool == index) g.hovered_tool = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ToolButtonProc, subclass_id);
        break;
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}

static void draw_tool_button(const DRAWITEMSTRUCT *item) {
    if (!item || item->CtlID < ID_TOOL_SELECT || item->CtlID > ID_TOOL_CLICK)
        return;
    int index = (int)item->CtlID - ID_TOOL_SELECT;
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
    BOOL selected = index == (int)g.tool;
    BOOL hovered = index == g.hovered_tool && !disabled;

    COLORREF background = GetSysColor(COLOR_BTNFACE);
    COLORREF border = GetSysColor(COLOR_3DSHADOW);
    COLORREF foreground = disabled ? GetSysColor(COLOR_GRAYTEXT) :
                                      GetSysColor(COLOR_BTNTEXT);
    if (selected) {
        background = pressed ? RGB(194, 225, 237) : RGB(218, 239, 247);
        border = RGB(22, 131, 173);
        foreground = disabled ? GetSysColor(COLOR_GRAYTEXT) : RGB(7, 90, 122);
    } else if (pressed) {
        background = GetSysColor(COLOR_3DLIGHT);
        border = GetSysColor(COLOR_3DDKSHADOW);
    } else if (hovered) {
        background = GetSysColor(COLOR_3DHIGHLIGHT);
        border = GetSysColor(COLOR_HOTLIGHT);
    }

    UINT dpi = GetDpiForWindow(item->hwndItem);
    int radius = max(2, golden_scale_ui(4, dpi));
    HPEN pen = CreatePen(PS_SOLID, max(1, golden_scale_ui(1, dpi)), border);
    HBRUSH brush = CreateSolidBrush(background);
    int saved = SaveDC(item->hDC);
    if (!saved) {
        FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_BTNFACE));
        if (pen) DeleteObject(pen);
        if (brush) DeleteObject(brush);
        return;
    }
    FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_BTNFACE));
    if (pen && brush) {
        SelectObject(item->hDC, pen);
        SelectObject(item->hDC, brush);
        RoundRect(item->hDC, item->rcItem.left, item->rcItem.top,
                  item->rcItem.right, item->rcItem.bottom, radius, radius);
    } else {
        FillRect(item->hDC, &item->rcItem, GetSysColorBrush(COLOR_BTNFACE));
    }

    RECT icon_bounds = item->rcItem;
    if (pressed) OffsetRect(&icon_bounds, golden_scale_ui(1, dpi),
                            golden_scale_ui(1, dpi));
    golden_draw_tool_icon(item->hDC, (GoldenToolIcon)index, &icon_bounds,
                          foreground, dpi);

    if (item->itemState & ODS_FOCUS) {
        RECT focus = item->rcItem;
        InflateRect(&focus, -golden_scale_ui(3, dpi),
                    -golden_scale_ui(3, dpi));
        DrawFocusRect(item->hDC, &focus);
    }
    RestoreDC(item->hDC, saved);
    if (pen) DeleteObject(pen);
    if (brush) DeleteObject(brush);
}

static void layout_children(HWND hwnd) {
    RECT client;
    GetClientRect(hwnd, &client);
    GoldenUiLayout layout = golden_compute_ui_layout(
        client.right, client.bottom, GetDpiForWindow(hwnd),
        g.left_column_width, g.right_column_width,
        g.left_collapsed, g.right_collapsed);
    ShowWindow(g.tree, g.left_collapsed ? SW_HIDE : SW_SHOWNA);
    ShowWindow(g.windows, g.right_collapsed ? SW_HIDE : SW_SHOWNA);
    for (int i = 0; i < 2; ++i)
        ShowWindow(g.window_buttons[i], g.right_collapsed ? SW_HIDE : SW_SHOWNA);
#define PLACE_CONTROL(control, rectangle) \
    SetWindowPos((control), NULL, (rectangle).left, (rectangle).top, \
        (rectangle).right - (rectangle).left, (rectangle).bottom - (rectangle).top, \
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW)
    PLACE_CONTROL(g.tree, layout.resource_tree);
    PLACE_CONTROL(g.left_splitter, layout.left_splitter);
    for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i)
        PLACE_CONTROL(g.tool_buttons[i], layout.tool_buttons[i]);
    PLACE_CONTROL(g.context_label, layout.context_label);
    PLACE_CONTROL(g.editor, layout.editor);
    for (int i = 0; i < GOLDEN_VIEW_BUTTON_COUNT; ++i)
        PLACE_CONTROL(g.view_buttons[i], layout.view_buttons[i]);
    for (int i = 0; i < 2; ++i) PLACE_CONTROL(g.window_buttons[i], layout.window_buttons[i]);
    PLACE_CONTROL(g.right_splitter, layout.right_splitter);
    PLACE_CONTROL(g.windows, layout.window_tree);
    PLACE_CONTROL(g.status, layout.status);
#undef PLACE_CONTROL
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void set_tool_with_focus(ToolMode tool, BOOL focus_editor) {
    BOOL preview_available = g.preview_mode && selected_capture_window();
    BOOL resource_available = g.resource_visible &&
                              selected_active_resource_node() != NULL;
    if (!preview_available && !resource_available) {
        update_tool_availability();
        return;
    }
    if (g.preview_mode && tool != TOOL_SELECT) return;
    if (tool == TOOL_CLICK && (g.preview_mode || g.selected < 0 ||
                               g.selected >= g.annotation_count)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g.tool = tool;
    update_tool_availability();
    if (focus_editor) SetFocus(g.editor);
}

static void set_tool(ToolMode tool) {
    set_tool_with_focus(tool, TRUE);
}

static void update_tool_availability(void) {
    if (!g.tool_buttons[0]) return;
    BOOL window_selected = selected_capture_window() != NULL;
    BOOL resource_selected = g.resource_visible &&
                             selected_active_resource_node() != NULL;
    BOOL preview_available = g.preview_mode && window_selected;
    BOOL click_available = resource_selected && !g.preview_mode && g.selected >= 0 &&
                           g.selected < g.annotation_count;
    if (preview_available) g.tool = TOOL_SELECT;
    else if (!click_available && g.tool == TOOL_CLICK) g.tool = TOOL_SELECT;
    for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i) {
        BOOL available = preview_available ? i == TOOL_SELECT :
                         resource_selected && (i != TOOL_CLICK || click_available);
        EnableWindow(g.tool_buttons[i], available);
        InvalidateRect(g.tool_buttons[i], NULL, FALSE);
    }
}

static void zoom_by(double factor, const POINT *anchor) {
    if (!active_pixels() || !active_width() || !active_height()) return;
    RECT client;
    GetClientRect(g.editor, &client);
    GoldenViewport current = golden_compute_viewport(
        (int)active_width(), (int)active_height(), client.right, client.bottom,
        30, g.zoom, g.pan_x, g.pan_y);
    double next_zoom = min(8.0, max(0.05, current.scale * factor));
    if (next_zoom == current.scale) return;
    POINT zoom_anchor = anchor ? *anchor : (POINT){
        (current.destination.left + current.destination.right) / 2,
        (current.destination.top + current.destination.bottom) / 2
    };
    GoldenViewport centered_zoom = golden_compute_viewport(
        (int)active_width(), (int)active_height(), client.right, client.bottom,
        30, next_zoom, 0, 0);
    POINT pan = golden_zoom_anchor_pan(&current, &centered_zoom, zoom_anchor);
    g.zoom = next_zoom;
    g.pan_x = pan.x;
    g.pan_y = pan.y;
    InvalidateRect(g.editor, NULL, FALSE);
}

static void handle_command(int id) {
    switch (id) {
    case ID_OPEN: open_folder(); break;
    case ID_SAVE: save_annotations(); break;
    case ID_EXIT: SendMessageW(g.main, WM_CLOSE, 0, 0); break;
    case ID_UNDO: undo_action(); break;
    case ID_REDO: redo_action(); break;
    case ID_RENAME: {
        HWND focus = GetFocus();
        HTREEITEM selected = TreeView_GetSelection(g.tree);
        if ((focus == g.tree || IsChild(g.tree, focus)) && begin_tree_rename(selected)) break;
        rename_selected();
        break;
    }
    case ID_DELETE: delete_selected(); break;
    case ID_CLEAR_CLICK: clear_click(); break;
    case ID_FIT: g.zoom = 0.0; g.pan_x = g.pan_y = 0; InvalidateRect(g.editor, NULL, FALSE); break;
    case ID_ZOOM_OUT: zoom_by(0.8, NULL); break;
    case ID_ZOOM_IN: zoom_by(1.25, NULL); break;
    case ID_ACTUAL: g.zoom = 1.0; g.pan_x = g.pan_y = 0; InvalidateRect(g.editor, NULL, FALSE); break;
    case ID_CAPTURE: capture_new(); break;
    case ID_RECAPTURE: recapture_current(); break;
    case ID_REFRESH: {
        refresh_resources();
        update_status();
        break;
    }
    case ID_TOOL_SELECT: set_tool(TOOL_SELECT); break;
    case ID_TOOL_RECTANGLE: set_tool(TOOL_RECTANGLE); break;
    case ID_TOOL_CLICK: set_tool(TOOL_CLICK); break;
    }
}

static HMENU create_main_menu(void) {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU edit = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU capture = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_OPEN, L"Open Folder…\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_SAVE, L"Save Annotations\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_EXIT, L"Exit");
    AppendMenuW(edit, MF_STRING, ID_UNDO, L"Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, ID_REDO, L"Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, ID_RENAME, L"Rename Selection\tF2");
    AppendMenuW(edit, MF_STRING, ID_DELETE, L"Delete Annotation\tDel");
    AppendMenuW(edit, MF_STRING, ID_CLEAR_CLICK, L"Clear Click Point");
    AppendMenuW(view, MF_STRING, ID_FIT, L"Fit Image\t0");
    AppendMenuW(view, MF_STRING, ID_ZOOM_OUT, L"Zoom Out\tCtrl+-");
    AppendMenuW(view, MF_STRING, ID_ZOOM_IN, L"Zoom In\tCtrl++");
    AppendMenuW(view, MF_STRING, ID_ACTUAL, L"Actual Size\t1");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, ID_REFRESH, L"Rescan Resource Folder\tF5");
    AppendMenuW(capture, MF_STRING, ID_CAPTURE, L"New Capture…");
    AppendMenuW(capture, MF_STRING, ID_RECAPTURE, L"Recapture Current Resource");
    g.capture_menu = capture;
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)capture, L"Capture");
    return bar;
}

static void activate_resource_node(ResourceTreeNode *node) {
    if (node && node->kind == RESOURCE_DIRECTORY && node->path) {
        if (!golden_path_copy(node->path, g.current_dir,
                              _countof(g.current_dir))) return;
        update_status();
    } else if (node && node->kind == RESOURCE_PNG && node->path) {
        if (!_wcsicmp(node->path, g.image_path)) {
            g.resource_visible = TRUE;
            clear_preview();
            g.selected = -1;
            update_tool_availability();
            update_context_label();
            InvalidateRect(g.editor, NULL, FALSE);
        } else if (!load_resource(node->path)) {
            sync_tree_annotation_selection();
        }
    } else if (node && node->kind == RESOURCE_ANNOTATION &&
               node->annotation_index >= 0 &&
               node->annotation_index < g.annotation_count) {
        g.resource_visible = TRUE;
        clear_preview();
        g.selected = node->annotation_index;
        set_tool_with_focus(TOOL_SELECT, FALSE);
        InvalidateRect(g.editor, NULL, FALSE);
    }
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.main = hwnd;
        if (!golden_preview_service_init(&g.preview_service, hwnd,
                WM_PREVIEW_READY, capture_preview_frame, NULL)) {
            MessageBoxW(hwnd,
                L"Goldens could not start its window preview service.\n\n"
                L"The application will close.",
                APP_NAME, MB_OK | MB_ICONERROR);
            return -1;
        }
        SetMenu(hwnd, create_main_menu());
        g.context_label = CreateWindowW(L"STATIC", L"  No resource selected", WS_CHILD | WS_VISIBLE |
            SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, NULL, g.instance, NULL);

        const wchar_t *tool_labels[] = {L"Select", L"Rectangle", L"Click"};
        const wchar_t *tool_tips[] = {L"Select, move, resize, or pan",
                                      L"Rectangle", L"Click point"};
        const int tool_ids[] = {ID_TOOL_SELECT, ID_TOOL_RECTANGLE, ID_TOOL_CLICK};
        for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i) {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
            if (!i) style |= WS_GROUP;
            g.tool_buttons[i] = CreateWindowW(L"BUTTON", tool_labels[i], style,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)tool_ids[i], g.instance, NULL);
            if (g.tool_buttons[i])
                SetWindowSubclass(g.tool_buttons[i], ToolButtonProc,
                                  (UINT_PTR)(i + 1), (DWORD_PTR)i);
        }
        g.tool_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, NULL, g.instance, NULL);
        if (g.tool_tooltip) {
            SetWindowPos(g.tool_tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g.tool_tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
            for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i) {
                TOOLINFOW *tool = &g.tool_button_tooltips[i];
                ZeroMemory(tool, sizeof(*tool));
                tool->cbSize = TTTOOLINFO_V1_SIZE;
                tool->uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool->hwnd = hwnd;
                tool->uId = (UINT_PTR)g.tool_buttons[i];
                tool->hinst = g.instance;
                tool->lpszText = (wchar_t *)tool_tips[i];
                SendMessageW(g.tool_tooltip, TTM_ADDTOOLW, 0, (LPARAM)tool);
            }
        }
        const wchar_t *view_labels[] = {L"Fit", L"−", L"+"};
        const int view_ids[] = {ID_FIT, ID_ZOOM_OUT, ID_ZOOM_IN};
        for (int i = 0; i < GOLDEN_VIEW_BUTTON_COUNT; ++i)
            g.view_buttons[i] = CreateWindowW(L"BUTTON", view_labels[i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
                (HMENU)(INT_PTR)view_ids[i], g.instance, NULL);
        const wchar_t *window_labels[] = {L"Capture", L"Recapture"};
        const int window_ids[] = {ID_CAPTURE, ID_RECAPTURE};
        for (int i = 0; i < 2; ++i)
            g.window_buttons[i] = CreateWindowW(L"BUTTON", window_labels[i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
                (HMENU)(INT_PTR)window_ids[i], g.instance, NULL);
        g.tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_EDITLABELS, 0, 0, 0, 0, hwnd,
            (HMENU)ID_TREE, g.instance, NULL);
        g.editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"GoldensEditor", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
            (HMENU)ID_EDITOR, g.instance, NULL);
        g.editor_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_NOANIMATE | TTS_NOFADE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, hwnd, NULL, g.instance, NULL);
        if (g.editor_tooltip) {
            SetWindowPos(g.editor_tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g.editor_tooltip, TTM_SETMAXTIPWIDTH, 0,
                         golden_scale_ui(360, GetDpiForWindow(hwnd)));
            if (!golden_tooltip_register_tracking(g.editor_tooltip, g.editor,
                    g.instance, g.tooltip_text, &g.editor_tooltip_tool)) {
                DestroyWindow(g.editor_tooltip);
                g.editor_tooltip = NULL;
            }
        }
        g.windows = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd,
            (HMENU)ID_WINDOWS, g.instance, NULL);
        g.status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0, hwnd, NULL, g.instance, NULL);
        g.left_splitter = CreateWindowW(L"GoldensSplitter", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_SPLITTER_LEFT, g.instance, NULL);
        g.right_splitter = CreateWindowW(L"GoldensSplitter", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_SPLITTER_RIGHT, g.instance, NULL);
        set_tool(TOOL_SELECT);
        update_context_label();
        if (g.root[0]) {
            refresh_resources();
        }
        update_status();
        refresh_windows();
        update_capture_availability();
        SetTimer(hwnd, WINDOW_TIMER, 750, NULL);
        SetTimer(hwnd, PREVIEW_TIMER, PREVIEW_INTERVAL_MS, NULL);
        return 0;
    }
    case WM_SIZE:
        layout_children(hwnd);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = golden_scale_ui(800, GetDpiForWindow(hwnd));
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = golden_scale_ui(480, GetDpiForWindow(hwnd));
        return 0;
    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lp;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_TIMER:
        if (wp == WINDOW_TIMER && !g.panning) {
            refresh_windows();
            refresh_preview_metadata();
        }
        else if (wp == PREVIEW_TIMER) request_preview_frame();
        return 0;
    case WM_COMMAND:
        handle_command(LOWORD(wp));
        return 0;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        if (item && item->CtlID >= ID_TOOL_SELECT && item->CtlID <= ID_TOOL_CLICK) {
            draw_tool_button(item);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g.context_label) {
            SetBkMode((HDC)wp, TRANSPARENT);
            SetTextColor((HDC)wp, g.preview_mode ? RGB(190, 90, 0) :
                         g.resource_visible && g.image_path[0] ? RGB(0, 105, 145) :
                         GetSysColor(COLOR_BTNTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_PREVIEW_READY: {
        GoldenImage image = {0};
        GoldenPreviewCompletion completion = golden_preview_service_complete(
            &g.preview_service, g.preview_target, g.preview_generation, &image);
        if (completion == GOLDEN_PREVIEW_COMPLETION_ACCEPTED ||
            completion == GOLDEN_PREVIEW_COMPLETION_FAILED) {
            g.preview_image = image;
            if (completion == GOLDEN_PREVIEW_COMPLETION_ACCEPTED)
                ++g.image_revision;
            g.preview_loading = FALSE;
            InvalidateRect(g.editor, NULL, FALSE);
        }
        return 0;
    }
    case WM_RESOURCE_RENAMED: {
        refresh_resources();
        if (g.pending_resource_selection[0]) {
            HTREEITEM item = find_resource_item(TreeView_GetRoot(g.tree),
                                                g.pending_resource_selection);
            g.rebuilding_resources = TRUE;
            if (item) {
                TreeView_EnsureVisible(g.tree, item);
                TreeView_SelectItem(g.tree, item);
            }
            g.rebuilding_resources = FALSE;
            g.pending_resource_selection[0] = 0;
        }
        update_capture_availability();
        update_tool_availability();
        return 0;
    }
    case WM_ANNOTATION_RENAMED:
        refresh_annotation_tree();
        return 0;
    case WM_BEGIN_TREE_RENAME:
        begin_tree_rename((HTREEITEM)lp);
        return 0;
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)lp;
        if (header->idFrom == ID_TREE && header->code == TVN_BEGINLABELEDITW) {
            NMTVDISPINFOW *edit = (NMTVDISPINFOW *)lp;
            ResourceTreeNode *node = tree_node_data(edit->item.hItem);
            return !(node && (node->kind == RESOURCE_PNG ||
                              node->kind == RESOURCE_ANNOTATION));
        }
        if (header->idFrom == ID_TREE && header->code == TVN_ENDLABELEDITW) {
            NMTVDISPINFOW *edit = (NMTVDISPINFOW *)lp;
            ResourceTreeNode *node = tree_node_data(edit->item.hItem);
            if (!node || !edit->item.pszText) return FALSE;
            if (node->kind == RESOURCE_PNG && node->path)
                return rename_resource_file(node->path, edit->item.pszText);
            if (node->kind == RESOURCE_ANNOTATION &&
                node->annotation_index >= 0 && node->annotation_index < g.annotation_count) {
                wchar_t name[128];
                wcsncpy(name, edit->item.pszText, _countof(name) - 1);
                name[_countof(name) - 1] = 0;
                trim_text(name);
                if (!name[0]) { show_error(L"The annotation name cannot be empty."); return FALSE; }
                if (annotation_name_exists(name, node->annotation_index)) {
                    show_error(L"That annotation name is already used in this image.");
                    return FALSE;
                }
                if (wcscmp(name, g.annotations[node->annotation_index].name)) {
                    g.selected = node->annotation_index;
                    push_undo();
                    wcscpy(g.annotations[node->annotation_index].name, name);
                    g.dirty = TRUE;
                    update_tool_availability();
                    InvalidateRect(g.editor, NULL, FALSE);
                    PostMessageW(g.main, WM_ANNOTATION_RENAMED, 0, 0);
                }
                return TRUE;
            }
            return FALSE;
        }
        if (header->idFrom == ID_TREE && header->code == TVN_SELCHANGEDW &&
            !g.rebuilding_resources) {
            NMTREEVIEWW *change = (NMTREEVIEWW *)lp;
            ResourceTreeNode *node = (ResourceTreeNode *)change->itemNew.lParam;
            if (node) activate_resource_node(node);
            else {
                BOOL was_showing_resource = !g.preview_mode && g.resource_visible;
                g.resource_visible = FALSE;
                g.selected = -1;
                update_tool_availability();
                if (was_showing_resource) {
                    HWND target = selected_capture_window();
                    if (target) preview_window(target);
                    else {
                        update_context_label();
                        InvalidateRect(g.editor, NULL, FALSE);
                    }
                }
            }
            update_capture_availability();
            update_tool_availability();
        }
        if (header->idFrom == ID_WINDOWS && header->code == TVN_SELCHANGEDW && !g.rebuilding_windows) {
            HWND target = selected_capture_window();
            if (target) preview_window(target);
            else if (g.preview_mode) {
                ResourceTreeNode *resource = selected_active_resource_node();
                if (resource) activate_resource_node(resource);
                else {
                    g.resource_visible = FALSE;
                    clear_preview();
                    InvalidateRect(g.editor, NULL, FALSE);
                }
            }
            update_capture_availability();
            update_tool_availability();
        }
        if (header->idFrom == ID_WINDOWS && header->code == NM_CLICK) {
            HWND target = clicked_capture_window();
            if (target && (!g.preview_mode || target != g.preview_target))
                preview_window(target);
        }
        if (header->idFrom == ID_TREE && header->code == NM_CLICK) {
            ResourceTreeNode *node = clicked_resource_node();
            BOOL current_png = node && node->kind == RESOURCE_PNG && node->path &&
                               !_wcsicmp(node->path, g.image_path);
            BOOL current_annotation = node && node->kind == RESOURCE_ANNOTATION &&
                node->annotation_index >= 0 && node->annotation_index < g.annotation_count;
            if (current_png || current_annotation) activate_resource_node(node);
        }
        if (header->code == NM_CLICK &&
            (header->idFrom == ID_TREE || header->idFrom == ID_WINDOWS))
            clear_tree_selection_on_blank_click(header->hwndFrom);
        if (header->idFrom == ID_TREE && header->code == NM_DBLCLK) {
            DWORD position = GetMessagePos();
            TVHITTESTINFO hit = {0};
            hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
            ScreenToClient(g.tree, &hit.pt);
            TreeView_HitTest(g.tree, &hit);
            if (hit.hItem && (hit.flags & TVHT_ONITEMLABEL))
                PostMessageW(g.main, WM_BEGIN_TREE_RENAME, 0, (LPARAM)hit.hItem);
        }
        return 0;
    }
    case WM_CLOSE:
        if (maybe_save()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, WINDOW_TIMER);
        KillTimer(hwnd, PREVIEW_TIMER);
        if (g.editor_tooltip) {
            DestroyWindow(g.editor_tooltip);
            g.editor_tooltip = NULL;
        }
        if (g.tool_tooltip) {
            DestroyWindow(g.tool_tooltip);
            g.tool_tooltip = NULL;
        }
        free_tree_item(g.tree, TreeView_GetRoot(g.tree));
        clear_preview();
        golden_preview_service_shutdown(&g.preview_service, 2000);
        clear_image();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    initialize_app_state(instance);
    initialize_startup_root();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&g.wic))) {
        CoUninitialize();
        return 1;
    }
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_TREEVIEW_CLASSES |
        ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW editor_class = {0};
    editor_class.style = CS_DBLCLKS;
    editor_class.lpfnWndProc = EditorProc;
    editor_class.hInstance = instance;
    editor_class.hCursor = LoadCursorW(NULL, IDC_CROSS);
    editor_class.hbrBackground = (HBRUSH)(COLOR_APPWORKSPACE + 1);
    editor_class.lpszClassName = L"GoldensEditor";
    RegisterClassW(&editor_class);
    WNDCLASSW main_class = {0};
    main_class.lpfnWndProc = MainProc;
    main_class.hInstance = instance;
    main_class.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    main_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    main_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    main_class.lpszClassName = L"GoldensMain";
    RegisterClassW(&main_class);
    WNDCLASSW splitter_class = {0};
    splitter_class.style = CS_DBLCLKS;
    splitter_class.lpfnWndProc = SplitterProc;
    splitter_class.hInstance = instance;
    splitter_class.hCursor = LoadCursorW(NULL, IDC_SIZEWE);
    splitter_class.lpszClassName = L"GoldensSplitter";
    RegisterClassW(&splitter_class);
    g.main = CreateWindowExW(0, L"GoldensMain", APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 800, NULL, NULL, instance, NULL);
    if (!g.main) {
        IWICImagingFactory_Release(g.wic);
        CoUninitialize();
        return 1;
    }
    ShowWindow(g.main, show);
    UpdateWindow(g.main);
    ACCEL shortcuts[] = {
        {FVIRTKEY | FCONTROL, 'O', ID_OPEN}, {FVIRTKEY | FCONTROL, 'S', ID_SAVE},
        {FVIRTKEY | FCONTROL, 'Z', ID_UNDO}, {FVIRTKEY | FCONTROL, 'Y', ID_REDO},
        {FVIRTKEY, VK_F5, ID_REFRESH}, {FVIRTKEY, VK_F2, ID_RENAME},
        {FVIRTKEY, VK_DELETE, ID_DELETE},
        {FVIRTKEY | FCONTROL, VK_OEM_MINUS, ID_ZOOM_OUT},
        {FVIRTKEY | FCONTROL, VK_OEM_PLUS, ID_ZOOM_IN},
        {FVIRTKEY | FCONTROL, VK_SUBTRACT, ID_ZOOM_OUT},
        {FVIRTKEY | FCONTROL, VK_ADD, ID_ZOOM_IN}
    };
    HACCEL accelerators = CreateAcceleratorTableW(shortcuts, _countof(shortcuts));
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(g.main, accelerators, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(accelerators);
    IWICImagingFactory_Release(g.wic);
    CoUninitialize();
    return (int)msg.wParam;
}
