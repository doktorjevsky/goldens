#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wincodec.h>
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
#include "editor_render.h"
#include "ui_layout.h"
#include "ui_tooltip.h"
#include "ui_tool_icon.h"
#include "resource_ops.h"

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
    ID_TOOL_SELECT, ID_TOOL_RECTANGLE, ID_TOOL_CLICK, ID_TOOL_HAND,
    ID_TREE = 200, ID_EDITOR, ID_WINDOWS, ID_SPLITTER_LEFT, ID_SPLITTER_RIGHT,
    ID_PROMPT_EDIT = 300, ID_PROMPT_OK, ID_PROMPT_CANCEL
};

typedef enum {
    TOOL_SELECT,
    TOOL_RECTANGLE,
    TOOL_CLICK,
    TOOL_HAND
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
    HWND target;
    LONG generation;
} PreviewRequest;

typedef struct {
    HWND target;
    LONG generation;
    GoldenImage image;
} PreviewResult;

static HINSTANCE g_instance;
static HWND g_main, g_tree, g_editor, g_windows, g_status;
static HWND g_editor_tooltip, g_tool_tooltip;
static HWND g_left_splitter, g_right_splitter;
static HWND g_context_label;
static HWND g_tool_buttons[4], g_view_buttons[GOLDEN_VIEW_BUTTON_COUNT], g_window_buttons[2];
static TOOLINFOW g_tool_button_tooltips[4];
static IWICImagingFactory *g_wic;
static wchar_t g_root[MAX_PATH * 4];
static wchar_t g_image_path[MAX_PATH * 4];
static wchar_t g_current_dir[MAX_PATH * 4];
static BYTE *g_pixels;
static UINT g_image_w, g_image_h, g_stride;
static GoldenImage g_preview_image;
static HWND g_preview_target;
static wchar_t g_preview_title[256];
static BOOL g_preview_mode;
static BOOL g_resource_visible;
static BOOL g_preview_loading;
static LONG g_preview_generation;
static double g_zoom;
static int g_pan_x, g_pan_y;
static BOOL g_panning;
static POINT g_pan_start;
static int g_pan_origin_x, g_pan_origin_y;
static Annotation g_annotations[MAX_ANNOTATIONS];
static int g_annotation_count;
static int g_selected = -1;
static BOOL g_dirty;
static Snapshot g_undo[MAX_HISTORY], g_redo[MAX_HISTORY];
static int g_undo_count, g_redo_count;
static int g_drag_mode;
static POINT g_drag_start;
static RECT g_drag_original;
static BOOL g_drawing;
static POINT g_draw_start, g_draw_current;
static ToolMode g_tool = TOOL_SELECT;
static ToolMode g_tool_before_preview = TOOL_SELECT;
static GoldenWindowInfo g_window_items[MAX_WINDOWS];
static int g_window_count;
static BOOL g_rebuilding_windows;
static BOOL g_rebuilding_resources;
static wchar_t g_pending_resource_selection[MAX_PATH * 4];
static int g_left_column_width = GOLDEN_RESOURCE_PANE_DEFAULT;
static int g_right_column_width = GOLDEN_WINDOWS_PANE_DEFAULT;
static BOOL g_left_collapsed, g_right_collapsed;
static BOOL g_splitter_dragging;
static POINT g_splitter_drag_start;
static int g_splitter_width_start;
static int g_tooltip_pending = -1;
static int g_tooltip_visible = -1;
static BOOL g_editor_mouse_tracking;
static wchar_t g_tooltip_text[128];
static TOOLINFOW g_editor_tooltip_tool;
static int g_hovered_tool = -1;

static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK EditorProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK PromptProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SplitterProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ToolButtonProc(HWND, UINT, WPARAM, LPARAM,
                                       UINT_PTR, DWORD_PTR);
static BOOL prompt_text(HWND owner, const wchar_t *title, const wchar_t *label,
                        wchar_t *value, size_t capacity);
static void preview_window(HWND target);
static HWND selected_capture_window(void);
static void refresh_annotation_tree(void);
static void update_context_label(void);
static void zoom_by(double factor);
static void update_tool_availability(void);
static void set_tool_with_focus(ToolMode tool, BOOL focus_editor);
static void set_tool(ToolMode tool);
static void layout_children(HWND hwnd);
static void hide_annotation_tooltip(HWND hwnd);
static void update_annotation_hover(HWND hwnd, POINT client);
static void draw_tool_button(const DRAWITEMSTRUCT *item);

static void show_error(const wchar_t *message) {
    MessageBoxW(g_main, message, APP_NAME, MB_OK | MB_ICONERROR);
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

static void json_path_for(const wchar_t *png, wchar_t *out, size_t cap) {
    golden_resource_json_path(png, out, cap);
}

static void parent_dir_for(const wchar_t *path, wchar_t *out, size_t cap) {
    wcsncpy(out, path, cap - 1);
    out[cap - 1] = 0;
    PathRemoveFileSpecW(out);
}

static void update_status(void) {
    if (!g_status) return;
    wchar_t text[MAX_PATH * 4 + 64];
    const wchar_t *folder = g_current_dir[0] ? g_current_dir : g_root;
    _snwprintf(text, _countof(text), L"  Capture folder: %s", folder[0] ? folder : L"(unavailable)");
    SetWindowTextW(g_status, text);
}

static void update_context_label(void) {
    if (!g_context_label) return;
    wchar_t text[MAX_PATH * 4 + 64];
    if (g_preview_mode)
        _snwprintf(text, _countof(text), L"  Previewing window  —  %s",
                   g_preview_title[0] ? g_preview_title : L"Untitled window");
    else if (g_resource_visible && g_image_path[0])
        _snwprintf(text, _countof(text), L"  Editing resource  —  %s",
                   PathFindFileNameW(g_image_path));
    else
        wcscpy(text, L"  No resource selected");
    SetWindowTextW(g_context_label, text);
}

static void snapshot_current(Snapshot *s) {
    s->count = g_annotation_count;
    s->selected = g_selected;
    memcpy(s->items, g_annotations, sizeof(Annotation) * g_annotation_count);
}

static void restore_snapshot(const Snapshot *s) {
    g_annotation_count = s->count;
    g_selected = s->selected;
    memcpy(g_annotations, s->items, sizeof(Annotation) * s->count);
    g_dirty = TRUE;
    update_tool_availability();
    refresh_annotation_tree();
    InvalidateRect(g_editor, NULL, FALSE);
}

static void push_undo(void) {
    if (g_undo_count == MAX_HISTORY) {
        memmove(&g_undo[0], &g_undo[1], sizeof(Snapshot) * (MAX_HISTORY - 1));
        g_undo_count--;
    }
    snapshot_current(&g_undo[g_undo_count++]);
    g_redo_count = 0;
}

static void undo_action(void) {
    if (!g_undo_count) return;
    if (g_redo_count < MAX_HISTORY) snapshot_current(&g_redo[g_redo_count++]);
    restore_snapshot(&g_undo[--g_undo_count]);
}

static void redo_action(void) {
    if (!g_redo_count) return;
    if (g_undo_count < MAX_HISTORY) snapshot_current(&g_undo[g_undo_count++]);
    restore_snapshot(&g_redo[--g_redo_count]);
}

static BOOL annotation_name_exists(const wchar_t *name, int except) {
    return golden_name_exists(g_annotations, g_annotation_count, name, except);
}

static void make_unique_name(wchar_t *out, size_t cap) {
    golden_make_unique_name(g_annotations, g_annotation_count, out, cap);
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
        if (!prompt_text(g_main, except < 0 ? L"New annotation" : L"Rename annotation",
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
        wc.hInstance = g_instance;
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
        owner, NULL, g_instance, &state);
    CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE, 16, 14, 390, 20,
                  state.window, NULL, g_instance, NULL);
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", value,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 38, 390, 25,
        state.window, (HMENU)ID_PROMPT_EDIT, g_instance, NULL);
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                  236, 79, 80, 28, state.window, (HMENU)ID_PROMPT_OK, g_instance, NULL);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                  326, 79, 80, 28, state.window, (HMENU)ID_PROMPT_CANCEL, g_instance, NULL);
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
    free(g_pixels);
    g_pixels = NULL;
    g_image_w = g_image_h = g_stride = 0;
    g_resource_visible = FALSE;
}

static void clear_preview(void) {
    BOOL was_previewing = g_preview_mode;
    InterlockedIncrement(&g_preview_generation);
    golden_image_free(&g_preview_image);
    g_preview_target = NULL;
    g_preview_title[0] = 0;
    g_preview_mode = FALSE;
    g_preview_loading = FALSE;
    if (was_previewing) g_tool = g_tool_before_preview;
    update_context_label();
    update_tool_availability();
}

static BYTE *active_pixels(void) {
    return g_preview_mode ? g_preview_image.pixels : g_resource_visible ? g_pixels : NULL;
}
static UINT active_width(void) {
    return g_preview_mode ? g_preview_image.width : g_resource_visible ? g_image_w : 0;
}
static UINT active_height(void) {
    return g_preview_mode ? g_preview_image.height : g_resource_visible ? g_image_h : 0;
}

static BOOL load_png(const wchar_t *path) {
    GoldenImage image = {0};
    if (!golden_png_load(g_wic, path, &image)) return FALSE;
    clear_image();
    g_pixels = image.pixels;
    g_image_w = image.width;
    g_image_h = image.height;
    g_stride = image.stride;
    return TRUE;
}

static BOOL save_png_pixels(const wchar_t *path, BYTE *pixels, UINT width, UINT height, UINT stride) {
    return golden_png_save(g_wic, path, pixels, width, height, stride);
}

static void load_annotations(const wchar_t *png_path) {
    g_annotation_count = 0;
    g_selected = -1;
    g_dirty = FALSE;
    g_undo_count = g_redo_count = 0;
    wchar_t path[MAX_PATH * 4];
    json_path_for(png_path, path, _countof(path));
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
    if (golden_document_parse_utf8(text, got, g_annotations, &count)) g_annotation_count = count;
    else show_error(L"The annotation JSON is invalid and was not loaded.");
    free(text);
}

static BOOL save_annotations(void) {
    if (!g_image_path[0]) return FALSE;
    wchar_t path[MAX_PATH * 4];
    json_path_for(g_image_path, path, _countof(path));
    size_t json_length = 0;
    char *json = golden_document_serialize_utf8(g_annotations, g_annotation_count, &json_length);
    if (!json) { show_error(L"Could not serialize the annotations."); return FALSE; }
    FILE *file = _wfopen(path, L"wb");
    if (!file) { free(json); show_error(L"Could not write the annotation JSON file."); return FALSE; }
    size_t written = fwrite(json, 1, json_length, file);
    int closed = fclose(file);
    BOOL ok = written == json_length && closed == 0;
    free(json);
    if (ok) g_dirty = FALSE;
    else show_error(L"Could not finish writing the annotation JSON file.");
    InvalidateRect(g_editor, NULL, FALSE);
    return ok;
}

static BOOL maybe_save(void) {
    if (!g_dirty) return TRUE;
    int answer = MessageBoxW(g_main, L"Save annotation changes?", APP_NAME,
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
    return TreeView_GetItem(g_tree, &info) ? (ResourceTreeNode *)info.lParam : NULL;
}

static HTREEITEM find_resource_item(HTREEITEM item, const wchar_t *path) {
    while (item) {
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->path && !_wcsicmp(node->path, path)) return item;
        HTREEITEM found = find_resource_item(TreeView_GetChild(g_tree, item), path);
        if (found) return found;
        item = TreeView_GetNextSibling(g_tree, item);
    }
    return NULL;
}

static HTREEITEM find_annotation_item(int annotation_index) {
    if (!g_tree || !g_image_path[0]) return NULL;
    HTREEITEM resource = find_resource_item(TreeView_GetRoot(g_tree), g_image_path);
    if (!resource) return NULL;
    for (HTREEITEM item = TreeView_GetChild(g_tree, resource); item;
         item = TreeView_GetNextSibling(g_tree, item)) {
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->kind == RESOURCE_ANNOTATION &&
            node->annotation_index == annotation_index) return item;
    }
    return NULL;
}

static void sync_tree_annotation_selection(void) {
    if (!g_tree || !g_image_path[0]) return;
    HTREEITEM target = g_selected >= 0 ? find_annotation_item(g_selected) :
        find_resource_item(TreeView_GetRoot(g_tree), g_image_path);
    if (!target) return;
    BOOL was_rebuilding = g_rebuilding_resources;
    g_rebuilding_resources = TRUE;
    TreeView_EnsureVisible(g_tree, target);
    TreeView_SelectItem(g_tree, target);
    g_rebuilding_resources = was_rebuilding;
}

static void delete_annotation_nodes(HTREEITEM item) {
    while (item) {
        HTREEITEM next = TreeView_GetNextSibling(g_tree, item);
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->kind == RESOURCE_ANNOTATION) {
            free(node->path);
            free(node);
            TreeView_DeleteItem(g_tree, item);
        } else {
            delete_annotation_nodes(TreeView_GetChild(g_tree, item));
        }
        item = next;
    }
}

static void refresh_annotation_tree(void) {
    if (!g_tree) return;
    g_rebuilding_resources = TRUE;
    delete_annotation_nodes(TreeView_GetRoot(g_tree));
    if (g_image_path[0]) {
        HTREEITEM resource = find_resource_item(TreeView_GetRoot(g_tree), g_image_path);
        HTREEITEM selected_annotation = NULL;
        for (int i = 0; resource && i < g_annotation_count; ++i) {
            ResourceTreeNode *node = (ResourceTreeNode *)calloc(1, sizeof(*node));
            if (!node) break;
            node->kind = RESOURCE_ANNOTATION;
            node->annotation_index = i;
            TVINSERTSTRUCTW insert = {0};
            insert.hParent = resource;
            insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            insert.item.pszText = g_annotations[i].name;
            insert.item.lParam = (LPARAM)node;
            HTREEITEM child = TreeView_InsertItem(g_tree, &insert);
            if (!child) free(node);
            else if (i == g_selected) selected_annotation = child;
        }
        if (resource) TreeView_Expand(g_tree, resource, TVE_EXPAND);
        if (selected_annotation) TreeView_SelectItem(g_tree, selected_annotation);
    }
    g_rebuilding_resources = FALSE;
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
    _snwprintf(pattern, _countof(pattern), L"%s\\*", directory);
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
        _snwprintf(full, _countof(full), L"%s\\%s", directory, data.cFileName);
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
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
        HTREEITEM node = insert_path_item(g_tree, parent, entries[i].name,
                                          entries[i].path, entries[i].directory);
        if (entries[i].directory) populate_directory(node, entries[i].path);
        free(entries[i].name);
        free(entries[i].path);
    }
    free(entries);
}

static void refresh_resources(void) {
    g_rebuilding_resources = TRUE;
    free_tree_item(g_tree, TreeView_GetRoot(g_tree));
    TreeView_DeleteAllItems(g_tree);
    if (g_root[0]) {
        const wchar_t *label = PathFindFileNameW(g_root);
        if (!*label) label = g_root;
        HTREEITEM root = insert_path_item(g_tree, TVI_ROOT, label, g_root, TRUE);
        populate_directory(root, g_root);
        TreeView_Expand(g_tree, root, TVE_EXPAND);
    }
    g_rebuilding_resources = FALSE;
    refresh_annotation_tree();
}

static void remember_root(void) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Goldens", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"LastFolder", 0, REG_SZ, (BYTE *)g_root,
                       (DWORD)((wcslen(g_root) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void load_remembered_root(void) {
    HKEY key;
    DWORD type, bytes = sizeof(g_root);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Goldens", 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"LastFolder", NULL, &type, (BYTE *)g_root, &bytes) != ERROR_SUCCESS ||
            type != REG_SZ || GetFileAttributesW(g_root) == INVALID_FILE_ATTRIBUTES) g_root[0] = 0;
        RegCloseKey(key);
    }
}

static void initialize_startup_root(void) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        DWORD attrs = GetFileAttributesW(argv[1]);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            GetFullPathNameW(argv[1], _countof(g_root), g_root, NULL);
    }
    if (argv) LocalFree(argv);
    if (!g_root[0]) GetCurrentDirectoryW(_countof(g_root), g_root);
    DWORD attrs = GetFileAttributesW(g_root);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        g_root[0] = 0;
        load_remembered_root();
    }
    if (g_root[0]) {
        wcsncpy(g_current_dir, g_root, _countof(g_current_dir) - 1);
        g_current_dir[_countof(g_current_dir) - 1] = 0;
    }
}

static int CALLBACK browse_callback(HWND hwnd, UINT msg, LPARAM lp, LPARAM data) {
    if (msg == BFFM_INITIALIZED && g_root[0])
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, (LPARAM)g_root);
    return 0;
}

static void open_folder(void) {
    if (!maybe_save()) return;
    BROWSEINFOW bi = {0};
    bi.hwndOwner = g_main;
    bi.lpszTitle = L"Choose a golden resources folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = browse_callback;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t path[MAX_PATH * 4];
    if (SHGetPathFromIDListW(id, path)) {
        wcsncpy(g_root, path, _countof(g_root) - 1);
        g_root[_countof(g_root) - 1] = 0;
        wcsncpy(g_current_dir, g_root, _countof(g_current_dir) - 1);
        g_current_dir[_countof(g_current_dir) - 1] = 0;
        clear_image();
        clear_preview();
        g_image_path[0] = 0;
        g_annotation_count = 0;
        g_selected = -1;
        update_tool_availability();
        g_dirty = FALSE;
        g_undo_count = g_redo_count = 0;
        remember_root();
        refresh_resources();
        update_status();
        SetWindowTextW(g_main, APP_NAME);
        update_context_label();
        InvalidateRect(g_editor, NULL, FALSE);
    }
    CoTaskMemFree(id);
}

static BOOL load_resource(const wchar_t *path) {
    if (!maybe_save()) return FALSE;
    if (!load_png(path)) { show_error(L"Could not decode the selected PNG file."); return FALSE; }
    clear_preview();
    g_resource_visible = TRUE;
    g_zoom = 0.0;
    g_pan_x = g_pan_y = 0;
    wcsncpy(g_image_path, path, _countof(g_image_path) - 1);
    g_image_path[_countof(g_image_path) - 1] = 0;
    parent_dir_for(path, g_current_dir, _countof(g_current_dir));
    update_status();
    load_annotations(path);
    update_tool_availability();
    refresh_annotation_tree();
    update_context_label();
    InvalidateRect(g_editor, NULL, FALSE);
    wchar_t title[MAX_PATH * 4 + 32];
    _snwprintf(title, _countof(title), L"Goldens — %s", PathFindFileNameW(path));
    SetWindowTextW(g_main, title);
    return TRUE;
}

static void image_layout(HWND hwnd, RECT *dest, double *scale) {
    RECT client;
    GetClientRect(hwnd, &client);
    UINT width = active_width(), height = active_height();
    if ((!g_preview_mode && !active_pixels()) || !width || !height) {
        SetRectEmpty(dest); *scale = 1.0; return;
    }
    GoldenViewport viewport = golden_compute_viewport(width, height, client.right, client.bottom,
                                                       30, g_zoom, g_pan_x, g_pan_y);
    *dest = viewport.destination;
    *scale = viewport.scale;
}

static BOOL client_to_image(HWND hwnd, POINT client, POINT *image) {
    RECT dest;
    double scale;
    image_layout(hwnd, &dest, &scale);
    GoldenViewport viewport = {dest, scale};
    return golden_view_to_image(&viewport, client, active_width(), active_height(), image);
}

static RECT annotation_screen_rect(HWND hwnd, const RECT *boundary) {
    RECT dest;
    double scale;
    image_layout(hwnd, &dest, &scale);
    RECT r = {
        dest.left + (LONG)(boundary->left * scale),
        dest.top + (LONG)(boundary->top * scale),
        dest.left + (LONG)(boundary->right * scale),
        dest.top + (LONG)(boundary->bottom * scale)
    };
    return r;
}

static void hide_annotation_tooltip(HWND hwnd) {
    KillTimer(hwnd, EDITOR_TOOLTIP_TIMER);
    g_tooltip_pending = -1;
    if (g_tooltip_visible >= 0)
        golden_tooltip_hide(g_editor_tooltip, &g_editor_tooltip_tool);
    g_tooltip_visible = -1;
}

static void update_annotation_hover(HWND hwnd, POINT client) {
    if (!g_editor_mouse_tracking) {
        TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, hwnd, 0};
        g_editor_mouse_tracking = TrackMouseEvent(&tracking);
    }
    POINT image;
    int hit = -1;
    if (!g_preview_mode && !g_panning && !g_drag_mode && !g_drawing &&
        client_to_image(hwnd, client, &image))
        hit = golden_hit_annotation(g_annotations, g_annotation_count, image);
    GoldenTooltipHoverAction action = golden_tooltip_hover_action(
        hit, g_tooltip_pending, g_tooltip_visible);
    if (action == GOLDEN_TOOLTIP_HOVER_NONE) return;
    hide_annotation_tooltip(hwnd);
    if (action == GOLDEN_TOOLTIP_HOVER_SCHEDULE) {
        g_tooltip_pending = hit;
        SetTimer(hwnd, EDITOR_TOOLTIP_TIMER, EDITOR_TOOLTIP_DELAY_MS, NULL);
    }
}

static void show_pending_annotation_tooltip(HWND hwnd) {
    KillTimer(hwnd, EDITOR_TOOLTIP_TIMER);
    if (!g_editor_tooltip || g_tooltip_pending < 0 ||
        g_tooltip_pending >= g_annotation_count) return;
    POINT cursor;
    GetCursorPos(&cursor);
    POINT client = cursor;
    ScreenToClient(hwnd, &client);
    POINT image;
    int hit = client_to_image(hwnd, client, &image) ?
        golden_hit_annotation(g_annotations, g_annotation_count, image) : -1;
    if (hit != g_tooltip_pending || g_preview_mode || g_panning ||
        g_drag_mode || g_drawing) {
        g_tooltip_pending = -1;
        return;
    }
    wcsncpy(g_tooltip_text, g_annotations[hit].name, _countof(g_tooltip_text) - 1);
    g_tooltip_text[_countof(g_tooltip_text) - 1] = 0;
    POINT position = {cursor.x + 12, cursor.y + 20};
    golden_tooltip_show(g_editor_tooltip, &g_editor_tooltip_tool,
                        g_tooltip_text, position);
    g_tooltip_visible = hit;
    g_tooltip_pending = -1;
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
        if (g_preview_mode)
            message = g_preview_loading ? L"Capturing window preview…" : L"Preview unavailable for this window";
        else
            message = g_root[0] ? L"Select a PNG from the resource tree" : L"Open a resource folder to begin";
        DrawTextW(dc, message, -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    RECT dest;
    double scale;
    image_layout(hwnd, &dest, &scale);
    golden_draw_bgra_image(dc, pixels, image_w, image_h, &dest, scale);
    FrameRect(dc, &dest, (HBRUSH)GetStockObject(BLACK_BRUSH));
    for (int i = 0; !g_preview_mode && i < g_annotation_count; ++i) {
        RECT r = annotation_screen_rect(hwnd, &g_annotations[i].boundary);
        golden_draw_boundary(dc, &r,
            i == g_selected ? RGB(255, 180, 0) : RGB(0, 220, 255),
            i == g_selected ? 3 : 2, PS_SOLID);
        if (i == g_selected) {
            RECT handle = {r.right - 5, r.bottom - 5, r.right + 5, r.bottom + 5};
            FillRect(dc, &handle, (HBRUSH)GetStockObject(WHITE_BRUSH));
        }
        if (g_annotations[i].has_click) {
            int cx = r.left + (int)((r.right - r.left) * g_annotations[i].click_x);
            int cy = r.top + (int)((r.bottom - r.top) * g_annotations[i].click_y);
            MoveToEx(dc, cx - 6, cy, NULL); LineTo(dc, cx + 7, cy);
            MoveToEx(dc, cx, cy - 6, NULL); LineTo(dc, cx, cy + 7);
        }
    }
    if (!g_preview_mode && g_drawing) {
        RECT boundary = {min(g_draw_start.x, g_draw_current.x), min(g_draw_start.y, g_draw_current.y),
                         max(g_draw_start.x, g_draw_current.x), max(g_draw_start.y, g_draw_current.y)};
        RECT r = annotation_screen_rect(hwnd, &boundary);
        golden_fill_tinted_rect(dc, &r, RGB(255, 150, 0), 112);
        golden_draw_boundary(dc, &r, RGB(255, 210, 0), 3, PS_SOLID);
    }
    wchar_t info_text[512];
    if (g_preview_mode)
        _snwprintf(info_text, _countof(info_text), L"Window preview: %s  •  %u × %u px  •  %.0f%%",
                   g_preview_title, image_w, image_h, scale * 100.0);
    else
        _snwprintf(info_text, _countof(info_text), L"%u × %u px  •  %d annotation%s%s  •  %.0f%%",
            image_w, image_h, g_annotation_count, g_annotation_count == 1 ? L"" : L"s",
            g_dirty ? L"  •  Unsaved" : L"", scale * 100.0);
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
    HDC buffer_dc = CreateCompatibleDC(paint_dc);
    HBITMAP buffer = buffer_dc ? CreateCompatibleBitmap(paint_dc, width, height) : NULL;
    HGDIOBJ previous = NULL;
    if (buffer) previous = SelectObject(buffer_dc, buffer);
    HDC target = buffer ? buffer_dc : paint_dc;
    draw_editor(hwnd, target);
    if (buffer) BitBlt(paint_dc, 0, 0, width, height, buffer_dc, 0, 0, SRCCOPY);
    if (previous) SelectObject(buffer_dc, previous);
    if (buffer) DeleteObject(buffer);
    if (buffer_dc) DeleteDC(buffer_dc);
    EndPaint(hwnd, &ps);
}

static void rename_selected(void) {
    if (g_selected < 0) return;
    wchar_t name[128];
    wcscpy(name, g_annotations[g_selected].name);
    if (!prompt_annotation_name(name, _countof(name), g_selected)) return;
    push_undo();
    wcscpy(g_annotations[g_selected].name, name);
    g_dirty = TRUE;
    refresh_annotation_tree();
    InvalidateRect(g_editor, NULL, FALSE);
}

static void delete_selected(void) {
    if (g_selected < 0) return;
    push_undo();
    memmove(&g_annotations[g_selected], &g_annotations[g_selected + 1],
            sizeof(Annotation) * (g_annotation_count - g_selected - 1));
    g_annotation_count--;
    if (g_selected >= g_annotation_count) g_selected = g_annotation_count - 1;
    g_dirty = TRUE;
    update_tool_availability();
    refresh_annotation_tree();
    InvalidateRect(g_editor, NULL, FALSE);
}

static void clear_click(void) {
    if (g_selected < 0 || !g_annotations[g_selected].has_click) return;
    push_undo();
    g_annotations[g_selected].has_click = FALSE;
    g_dirty = TRUE;
    InvalidateRect(g_editor, NULL, FALSE);
}

static void deselect_annotation(void) {
    if (g_selected < 0) return;
    g_selected = -1;
    update_tool_availability();
    sync_tree_annotation_selection();
    InvalidateRect(g_editor, NULL, FALSE);
}

static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: paint_editor(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            LPCWSTR cursor = g_tool == TOOL_HAND || g_preview_mode ? IDC_SIZEALL :
                             g_tool == TOOL_SELECT ? IDC_ARROW : IDC_CROSS;
            SetCursor(LoadCursorW(NULL, cursor));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        hide_annotation_tooltip(hwnd);
        SetFocus(hwnd);
        if (g_preview_mode || g_tool == TOOL_HAND) {
            g_panning = TRUE;
            g_pan_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            g_pan_origin_x = g_pan_x; g_pan_origin_y = g_pan_y;
            SetCapture(hwnd);
            return 0;
        }
        POINT client = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, image;
        if (!client_to_image(hwnd, client, &image)) {
            if (g_tool == TOOL_SELECT) deselect_annotation();
            return 0;
        }
        if (g_tool == TOOL_CLICK) {
            if (g_selected >= 0 && g_selected < g_annotation_count &&
                PtInRect(&g_annotations[g_selected].boundary, image)) {
                push_undo();
                golden_set_click(&g_annotations[g_selected], image);
                g_dirty = TRUE;
            } else MessageBeep(MB_ICONWARNING);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_tool == TOOL_RECTANGLE) {
            if (g_annotation_count >= MAX_ANNOTATIONS) {
                show_error(L"The annotation limit has been reached.");
                return 0;
            }
            g_selected = -1;
            update_tool_availability();
            sync_tree_annotation_selection();
            g_drawing = TRUE;
            g_draw_start = g_draw_current = image;
            SetCapture(hwnd);
        } else {
            int hit = golden_hit_annotation(g_annotations, g_annotation_count, image);
            if (hit >= 0) {
                g_selected = hit;
                update_tool_availability();
                sync_tree_annotation_selection();
                RECT screen = annotation_screen_rect(hwnd, &g_annotations[hit].boundary);
                g_drag_mode = abs(client.x - screen.right) <= 9 && abs(client.y - screen.bottom) <= 9 ? 2 : 1;
                g_drag_start = image;
                g_drag_original = g_annotations[hit].boundary;
                push_undo();
                SetCapture(hwnd);
            } else {
                deselect_annotation();
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        update_annotation_hover(hwnd,
            (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (g_panning) {
            g_pan_x = g_pan_origin_x + GET_X_LPARAM(lp) - g_pan_start.x;
            g_pan_y = g_pan_origin_y + GET_Y_LPARAM(lp) - g_pan_start.y;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_drag_mode || g_drawing) {
            POINT client = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, image;
            RECT dest; double scale; image_layout(hwnd, &dest, &scale);
            image.x = (LONG)((client.x - dest.left) / scale);
            image.y = (LONG)((client.y - dest.top) / scale);
            image.x = min((LONG)g_image_w, max(0, image.x));
            image.y = min((LONG)g_image_h, max(0, image.y));
            if (g_drawing) g_draw_current = image;
            else if (g_selected >= 0) {
                RECT *r = &g_annotations[g_selected].boundary;
                if (g_drag_mode == 1) {
                    int dx = image.x - g_drag_start.x, dy = image.y - g_drag_start.y;
                    int width = g_drag_original.right - g_drag_original.left;
                    int height = g_drag_original.bottom - g_drag_original.top;
                    r->left = min((LONG)g_image_w - width, max(0, g_drag_original.left + dx));
                    r->top = min((LONG)g_image_h - height, max(0, g_drag_original.top + dy));
                    r->right = r->left + width; r->bottom = r->top + height;
                } else {
                    r->right = min((LONG)g_image_w, max(r->left + 1, image.x));
                    r->bottom = min((LONG)g_image_h, max(r->top + 1, image.y));
                }
                g_dirty = TRUE;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        g_editor_mouse_tracking = FALSE;
        hide_annotation_tooltip(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == EDITOR_TOOLTIP_TIMER) {
            show_pending_annotation_tooltip(hwnd);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_panning) {
            g_panning = FALSE;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_drag_mode) {
            g_drag_mode = 0;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g_drawing) {
            ReleaseCapture();
            g_drawing = FALSE;
            RECT r = golden_normalize_rect(g_draw_start, g_draw_current);
            if (r.right - r.left >= 2 && r.bottom - r.top >= 2) {
                wchar_t name[128];
                make_unique_name(name, _countof(name));
                if (prompt_annotation_name(name, _countof(name), -1)) {
                    push_undo();
                    Annotation *a = &g_annotations[g_annotation_count];
                    ZeroMemory(a, sizeof(*a));
                    wcscpy(a->name, name);
                    a->boundary = r;
                    g_selected = g_annotation_count++;
                    g_dirty = TRUE;
                    refresh_annotation_tree();
                    set_tool(TOOL_SELECT);
                }
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MBUTTONDOWN:
        g_panning = TRUE;
        g_pan_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_pan_origin_x = g_pan_x; g_pan_origin_y = g_pan_y;
        SetCapture(hwnd);
        return 0;
    case WM_MBUTTONUP:
        if (g_panning) { g_panning = FALSE; ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL: {
        zoom_by(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.25 : 0.8);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (!g_preview_mode && g_tool == TOOL_SELECT) rename_selected();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_DELETE) delete_selected();
        else if (wp == VK_F2) rename_selected();
        else if (wp == 'Z' && GetKeyState(VK_CONTROL) < 0) undo_action();
        else if (wp == 'Y' && GetKeyState(VK_CONTROL) < 0) redo_action();
        else if (wp == '0') { g_zoom = 0.0; g_pan_x = g_pan_y = 0; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == '1') { g_zoom = 1.0; g_pan_x = g_pan_y = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void refresh_windows(void) {
    GoldenWindowInfo next[MAX_WINDOWS];
    int next_count = golden_collect_windows(g_main, next, MAX_WINDOWS);
    if (golden_window_lists_equal(g_window_items, g_window_count, next, next_count)) return;
    HWND selected_window = selected_capture_window();
    g_rebuilding_windows = TRUE;
    TreeView_DeleteAllItems(g_windows);
    memcpy(g_window_items, next, sizeof(GoldenWindowInfo) * next_count);
    g_window_count = next_count;
    HTREEITEM group = NULL, selected_item = NULL;
    wchar_t previous[128] = L"";
    for (int i = 0; i < g_window_count; ++i) {
        if (_wcsicmp(previous, g_window_items[i].app)) {
            if (group) TreeView_Expand(g_windows, group, TVE_EXPAND);
            wcscpy(previous, g_window_items[i].app);
            TVINSERTSTRUCTW insert = {0};
            insert.hParent = TVI_ROOT; insert.hInsertAfter = TVI_LAST;
            insert.item.mask = TVIF_TEXT;
            insert.item.pszText = g_window_items[i].app;
            group = TreeView_InsertItem(g_windows, &insert);
        }
        TVINSERTSTRUCTW insert = {0};
        insert.hParent = group; insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = g_window_items[i].title;
        insert.item.lParam = (LPARAM)g_window_items[i].id;
        HTREEITEM leaf = TreeView_InsertItem(g_windows, &insert);
        if ((HWND)g_window_items[i].id == selected_window) selected_item = leaf;
    }
    if (group) TreeView_Expand(g_windows, group, TVE_EXPAND);
    if (selected_item) TreeView_SelectItem(g_windows, selected_item);
    g_rebuilding_windows = FALSE;
    if (g_preview_target) {
        BOOL found = FALSE;
        for (int i = 0; i < g_window_count; ++i)
            if ((HWND)g_window_items[i].id == g_preview_target) { found = TRUE; break; }
        if (!found) { clear_preview(); InvalidateRect(g_editor, NULL, FALSE); }
    }
}

static HWND selected_capture_window(void) {
    HTREEITEM selected = TreeView_GetSelection(g_windows);
    if (!selected) return NULL;
    TVITEMW item = {0};
    item.mask = TVIF_PARAM; item.hItem = selected;
    TreeView_GetItem(g_windows, &item);
    return (HWND)item.lParam;
}

static HWND clicked_capture_window(void) {
    DWORD position = GetMessagePos();
    TVHITTESTINFO hit = {0};
    hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(g_windows, &hit.pt);
    TreeView_HitTest(g_windows, &hit);
    if (!hit.hItem || !(hit.flags & TVHT_ONITEM)) return NULL;
    TVITEMW item = {0};
    item.mask = TVIF_PARAM;
    item.hItem = hit.hItem;
    if (!TreeView_GetItem(g_windows, &item)) return NULL;
    HWND target = (HWND)item.lParam;
    return target && IsWindow(target) ? target : NULL;
}

static ResourceTreeNode *clicked_resource_node(void) {
    DWORD position = GetMessagePos();
    TVHITTESTINFO hit = {0};
    hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(g_tree, &hit.pt);
    TreeView_HitTest(g_tree, &hit);
    if (!hit.hItem || !(hit.flags & TVHT_ONITEM)) return NULL;
    return tree_node_data(hit.hItem);
}

static ResourceTreeNode *selected_active_resource_node(void) {
    ResourceTreeNode *node = tree_node_data(TreeView_GetSelection(g_tree));
    if (!node) return NULL;
    if (node->kind == RESOURCE_PNG)
        return node->path && !_wcsicmp(node->path, g_image_path) ? node : NULL;
    if (node->kind == RESOURCE_ANNOTATION && node->annotation_index >= 0 &&
        node->annotation_index < g_annotation_count) return node;
    return NULL;
}

static void clear_tree_selection_on_blank_click(HWND tree) {
    DWORD position = GetMessagePos();
    TVHITTESTINFO hit = {0};
    hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(tree, &hit.pt);
    TreeView_HitTest(tree, &hit);
    if (hit.flags & (TVHT_ONITEM | TVHT_ONITEMBUTTON)) return;
    if (tree == g_tree) {
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

static DWORD WINAPI preview_worker(void *parameter) {
    PreviewRequest *request = (PreviewRequest *)parameter;
    PreviewResult *result = (PreviewResult *)calloc(1, sizeof(*result));
    if (result) {
        result->target = request->target;
        result->generation = request->generation;
        golden_capture_window_preview(request->target, &result->image);
        if (!PostMessageW(g_main, WM_PREVIEW_READY, 0, (LPARAM)result)) {
            golden_image_free(&result->image);
            free(result);
        }
    } else PostMessageW(g_main, WM_PREVIEW_READY, (WPARAM)request->generation, 0);
    free(request);
    return 0;
}

static void request_preview_frame(void) {
    if (!g_preview_mode || !g_preview_target || g_preview_loading || IsIconic(g_main)) return;
    if (!IsWindow(g_preview_target)) {
        clear_preview();
        InvalidateRect(g_editor, NULL, FALSE);
        return;
    }
    PreviewRequest *request = (PreviewRequest *)malloc(sizeof(*request));
    if (!request) return;
    request->target = g_preview_target;
    request->generation = g_preview_generation;
    g_preview_loading = TRUE;
    HANDLE thread = CreateThread(NULL, 0, preview_worker, request, 0, NULL);
    if (thread) CloseHandle(thread);
    else { free(request); g_preview_loading = FALSE; }
}

static void preview_window(HWND target) {
    if (!target || !IsWindow(target) || target == g_main) return;
    if (target == g_preview_target && g_preview_mode) {
        request_preview_frame();
        return;
    }
    clear_preview();
    g_tool_before_preview = g_tool;
    g_preview_target = target;
    GetWindowTextW(target, g_preview_title, _countof(g_preview_title));
    g_preview_mode = TRUE;
    update_tool_availability();
    g_zoom = 0.0;
    g_pan_x = g_pan_y = 0;
    update_context_label();
    request_preview_frame();
    InvalidateRect(g_editor, NULL, FALSE);
}

static void refresh_preview_metadata(void) {
    if (!g_preview_target) return;
    if (!IsWindow(g_preview_target)) {
        clear_preview();
        InvalidateRect(g_editor, NULL, FALSE);
        return;
    }
    wchar_t title[256] = L"";
    GetWindowTextW(g_preview_target, title, _countof(title));
    if (wcscmp(title, g_preview_title)) {
        wcscpy(g_preview_title, title);
        update_context_label();
        InvalidateRect(g_editor, NULL, FALSE);
    }
}

static BOOL capture_window_to(HWND target, const wchar_t *path) {
    if (!IsWindow(target)) { show_error(L"The selected window is no longer open."); return FALSE; }
    WINDOWPLACEMENT placement = {0};
    placement.length = sizeof(placement);
    GetWindowPlacement(target, &placement);
    BOOL was_minimized = IsIconic(target);
    ShowWindow(target, SW_RESTORE);
    ShowWindow(g_main, SW_MINIMIZE);
    SetForegroundWindow(target);
    Sleep(350);
    RECT rect;
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect))))
        GetWindowRect(target, &rect);
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    BOOL ok = FALSE;
    if (width > 0 && height > 0) {
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
            if (BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY | CAPTUREBLT))
                ok = save_png_pixels(path, bits, width, height, width * 4);
            SelectObject(memory, old);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(NULL, screen);
    }
    if (was_minimized) ShowWindow(target, SW_MINIMIZE);
    ShowWindow(g_main, SW_RESTORE);
    SetForegroundWindow(g_main);
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
    parent_dir_for(old_path, directory, _countof(directory));
    _snwprintf(new_path, _countof(new_path), L"%s\\%s.png", directory, name);
    if (!wcscmp(old_path, new_path)) return TRUE;

    GoldenResourceRenameResult result = golden_rename_resource_pair(old_path, new_path);
    if (result != GOLDEN_RENAME_OK) {
        const wchar_t *message = result == GOLDEN_RENAME_PNG_EXISTS ?
            L"A PNG with that resource name already exists in this directory." :
            result == GOLDEN_RENAME_JSON_EXISTS ?
            L"A JSON annotation file with that resource name already exists." :
            result == GOLDEN_RENAME_JSON_FAILED_ROLLED_BACK ?
            L"The JSON file could not be renamed, so the PNG rename was rolled back." :
            L"Windows could not rename the resource pair.";
        show_error(message);
        return FALSE;
    }
    if (!_wcsicmp(g_image_path, old_path)) {
        wcsncpy(g_image_path, new_path, _countof(g_image_path) - 1);
        g_image_path[_countof(g_image_path) - 1] = 0;
        update_context_label();
        wchar_t title[MAX_PATH * 4 + 32];
        _snwprintf(title, _countof(title), L"Goldens — %s", PathFindFileNameW(new_path));
        SetWindowTextW(g_main, title);
    }
    wcsncpy(g_pending_resource_selection, new_path,
            _countof(g_pending_resource_selection) - 1);
    g_pending_resource_selection[_countof(g_pending_resource_selection) - 1] = 0;
    PostMessageW(g_main, WM_RESOURCE_RENAMED, 0, 0);
    return TRUE;
}

static BOOL begin_tree_rename(HTREEITEM item) {
    ResourceTreeNode *node = tree_node_data(item);
    if (!node || (node->kind != RESOURCE_PNG && node->kind != RESOURCE_ANNOTATION))
        return FALSE;
    if (node->kind == RESOURCE_ANNOTATION &&
        node->annotation_index >= 0 && node->annotation_index < g_annotation_count) {
        clear_preview();
        g_selected = node->annotation_index;
        update_tool_availability();
        InvalidateRect(g_editor, NULL, FALSE);
    }
    TreeView_SelectItem(g_tree, item);
    HWND edit = TreeView_EditLabel(g_tree, item);
    if (edit) SendMessageW(edit, EM_LIMITTEXT,
        node->kind == RESOURCE_ANNOTATION ? 127 : 255, 0);
    return edit != NULL;
}

static BOOL ensure_capture_directory(void) {
    const wchar_t *candidate = g_current_dir[0] ? g_current_dir : g_root;
    DWORD attrs = GetFileAttributesW(candidate);
    if (candidate[0] && attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return TRUE;
    wchar_t cwd[MAX_PATH * 4];
    if (!GetCurrentDirectoryW(_countof(cwd), cwd)) return FALSE;
    attrs = GetFileAttributesW(cwd);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    wcsncpy(g_root, cwd, _countof(g_root) - 1);
    wcsncpy(g_current_dir, cwd, _countof(g_current_dir) - 1);
    g_root[_countof(g_root) - 1] = 0;
    g_current_dir[_countof(g_current_dir) - 1] = 0;
    refresh_resources();
    update_status();
    return TRUE;
}

static void capture_new(void) {
    HWND target = selected_capture_window();
    if (!target) { show_error(L"Select a window in the right column first."); return; }
    if (!ensure_capture_directory()) { show_error(L"The current directory is not available for captures."); return; }
    wchar_t name[128] = L"";
    if (!prompt_text(g_main, L"New screenshot", L"Resource name (without .png):", name, _countof(name))) return;
    normalize_capture_name(name);
    if (!valid_capture_name(name)) { show_error(L"Enter a valid Windows file name without path characters."); return; }
    const wchar_t *dir = g_current_dir[0] ? g_current_dir : g_root;
    wchar_t path[MAX_PATH * 4];
    _snwprintf(path, _countof(path), L"%s\\%s.png", dir, name);
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
    if (!g_image_path[0]) { show_error(L"Select an existing PNG to recapture."); return; }
    if (MessageBoxW(g_main, L"Replace the current PNG while preserving its annotations?",
                    APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    if (capture_window_to(target, g_image_path)) {
        clear_preview();
        if (load_png(g_image_path)) g_resource_visible = TRUE;
        g_zoom = 0.0;
        g_pan_x = g_pan_y = 0;
        InvalidateRect(g_editor, NULL, FALSE);
    }
}

static void toggle_splitter(HWND splitter) {
    if (splitter == g_left_splitter) g_left_collapsed = !g_left_collapsed;
    else if (splitter == g_right_splitter) g_right_collapsed = !g_right_collapsed;
    layout_children(g_main);
    InvalidateRect(g_left_splitter, NULL, TRUE);
    InvalidateRect(g_right_splitter, NULL, TRUE);
}

static LRESULT CALLBACK SplitterProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    BOOL left = hwnd == g_left_splitter;
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
        BOOL collapsed = left ? g_left_collapsed : g_right_collapsed;
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
        BOOL collapsed = left ? g_left_collapsed : g_right_collapsed;
        g_splitter_dragging = TRUE;
        g_splitter_drag_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &g_splitter_drag_start);
        g_splitter_width_start = 0;
        if (!collapsed) {
            RECT pane;
            GetWindowRect(left ? g_tree : g_windows, &pane);
            g_splitter_width_start = MulDiv(pane.right - pane.left, 96,
                                            (int)GetDpiForWindow(g_main));
        }
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_splitter_dragging && GetCapture() == hwnd) {
            POINT current = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &current);
            int delta = MulDiv(current.x - g_splitter_drag_start.x, 96,
                               (int)GetDpiForWindow(g_main));
            int width = left ? g_splitter_width_start + delta :
                               g_splitter_width_start - delta;
            int minimum = left ? GOLDEN_RESOURCE_PANE_MIN : GOLDEN_WINDOWS_PANE_MIN;
            int maximum = left ? 520 : 560;
            BOOL *collapsed = left ? &g_left_collapsed : &g_right_collapsed;
            int *preferred = left ? &g_left_column_width : &g_right_column_width;
            BOOL next_collapsed = golden_pane_should_collapse(width, minimum, *collapsed);
            if (next_collapsed != *collapsed) {
                *collapsed = next_collapsed;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            if (!next_collapsed) *preferred = max(minimum, min(maximum, width));
            layout_children(g_main);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_splitter_dragging && GetCapture() == hwnd) ReleaseCapture();
        g_splitter_dragging = FALSE;
        return 0;
    case WM_CAPTURECHANGED:
        g_splitter_dragging = FALSE;
        return 0;
    case WM_LBUTTONDBLCLK:
        if (GetCapture() == hwnd) ReleaseCapture();
        g_splitter_dragging = FALSE;
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
        if (IsWindowEnabled(hwnd) && g_hovered_tool != index) {
            int previous = g_hovered_tool;
            g_hovered_tool = index;
            if (previous >= 0 && previous < 4 && g_tool_buttons[previous])
                InvalidateRect(g_tool_buttons[previous], NULL, FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tracking = {sizeof(tracking), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tracking);
        }
        break;
    case WM_MOUSELEAVE:
        if (g_hovered_tool == index) {
            g_hovered_tool = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case WM_ENABLE:
        if (!wp && g_hovered_tool == index) g_hovered_tool = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ToolButtonProc, subclass_id);
        break;
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}

static void draw_tool_button(const DRAWITEMSTRUCT *item) {
    if (!item || item->CtlID < ID_TOOL_SELECT || item->CtlID > ID_TOOL_HAND)
        return;
    int index = (int)item->CtlID - ID_TOOL_SELECT;
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    BOOL pressed = (item->itemState & ODS_SELECTED) != 0;
    BOOL selected = index == (int)g_tool;
    BOOL hovered = index == g_hovered_tool && !disabled;

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
        g_left_column_width, g_right_column_width,
        g_left_collapsed, g_right_collapsed);
    ShowWindow(g_tree, g_left_collapsed ? SW_HIDE : SW_SHOWNA);
    ShowWindow(g_windows, g_right_collapsed ? SW_HIDE : SW_SHOWNA);
    for (int i = 0; i < 2; ++i)
        ShowWindow(g_window_buttons[i], g_right_collapsed ? SW_HIDE : SW_SHOWNA);
#define PLACE_CONTROL(control, rectangle) \
    SetWindowPos((control), NULL, (rectangle).left, (rectangle).top, \
        (rectangle).right - (rectangle).left, (rectangle).bottom - (rectangle).top, \
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW)
    PLACE_CONTROL(g_tree, layout.resource_tree);
    PLACE_CONTROL(g_left_splitter, layout.left_splitter);
    for (int i = 0; i < 4; ++i) PLACE_CONTROL(g_tool_buttons[i], layout.tool_buttons[i]);
    PLACE_CONTROL(g_context_label, layout.context_label);
    PLACE_CONTROL(g_editor, layout.editor);
    for (int i = 0; i < GOLDEN_VIEW_BUTTON_COUNT; ++i)
        PLACE_CONTROL(g_view_buttons[i], layout.view_buttons[i]);
    for (int i = 0; i < 2; ++i) PLACE_CONTROL(g_window_buttons[i], layout.window_buttons[i]);
    PLACE_CONTROL(g_right_splitter, layout.right_splitter);
    PLACE_CONTROL(g_windows, layout.window_tree);
    PLACE_CONTROL(g_status, layout.status);
#undef PLACE_CONTROL
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void set_tool_with_focus(ToolMode tool, BOOL focus_editor) {
    if (g_preview_mode && tool != TOOL_HAND) return;
    if (tool == TOOL_CLICK && (g_preview_mode || g_selected < 0 ||
                               g_selected >= g_annotation_count)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g_tool = tool;
    update_tool_availability();
    if (focus_editor) SetFocus(g_editor);
}

static void set_tool(ToolMode tool) {
    set_tool_with_focus(tool, TRUE);
}

static void update_tool_availability(void) {
    if (!g_tool_buttons[0]) return;
    BOOL click_available = !g_preview_mode && g_selected >= 0 &&
                           g_selected < g_annotation_count;
    if (g_preview_mode) g_tool = TOOL_HAND;
    else if (!click_available && g_tool == TOOL_CLICK) g_tool = TOOL_SELECT;
    for (int i = 0; i < 4; ++i) {
        BOOL available = g_preview_mode ? i == TOOL_HAND :
                         i != TOOL_CLICK || click_available;
        EnableWindow(g_tool_buttons[i], available);
        InvalidateRect(g_tool_buttons[i], NULL, FALSE);
    }
}

static void zoom_by(double factor) {
    if (!active_pixels() || !active_width() || !active_height()) return;
    RECT destination;
    double current_scale;
    image_layout(g_editor, &destination, &current_scale);
    g_zoom = min(8.0, max(0.05, current_scale * factor));
    g_pan_x = g_pan_y = 0;
    InvalidateRect(g_editor, NULL, FALSE);
}

static void handle_command(int id) {
    switch (id) {
    case ID_OPEN: open_folder(); break;
    case ID_SAVE: save_annotations(); break;
    case ID_EXIT: SendMessageW(g_main, WM_CLOSE, 0, 0); break;
    case ID_UNDO: undo_action(); break;
    case ID_REDO: redo_action(); break;
    case ID_RENAME: {
        HWND focus = GetFocus();
        HTREEITEM selected = TreeView_GetSelection(g_tree);
        if ((focus == g_tree || IsChild(g_tree, focus)) && begin_tree_rename(selected)) break;
        rename_selected();
        break;
    }
    case ID_DELETE: delete_selected(); break;
    case ID_CLEAR_CLICK: clear_click(); break;
    case ID_FIT: g_zoom = 0.0; g_pan_x = g_pan_y = 0; InvalidateRect(g_editor, NULL, FALSE); break;
    case ID_ZOOM_OUT: zoom_by(0.8); break;
    case ID_ZOOM_IN: zoom_by(1.25); break;
    case ID_ACTUAL: g_zoom = 1.0; g_pan_x = g_pan_y = 0; InvalidateRect(g_editor, NULL, FALSE); break;
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
    case ID_TOOL_HAND: set_tool(TOOL_HAND); break;
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
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)capture, L"Capture");
    return bar;
}

static void activate_resource_node(ResourceTreeNode *node) {
    if (node && node->kind == RESOURCE_DIRECTORY && node->path) {
        wcsncpy(g_current_dir, node->path, _countof(g_current_dir) - 1);
        g_current_dir[_countof(g_current_dir) - 1] = 0;
        update_status();
    } else if (node && node->kind == RESOURCE_PNG && node->path) {
        if (!_wcsicmp(node->path, g_image_path)) {
            g_resource_visible = TRUE;
            clear_preview();
            g_selected = -1;
            update_tool_availability();
            update_context_label();
            InvalidateRect(g_editor, NULL, FALSE);
        } else load_resource(node->path);
    } else if (node && node->kind == RESOURCE_ANNOTATION &&
               node->annotation_index >= 0 &&
               node->annotation_index < g_annotation_count) {
        g_resource_visible = TRUE;
        clear_preview();
        g_selected = node->annotation_index;
        set_tool_with_focus(TOOL_SELECT, FALSE);
        InvalidateRect(g_editor, NULL, FALSE);
    }
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        SetMenu(hwnd, create_main_menu());
        g_context_label = CreateWindowW(L"STATIC", L"  No resource selected", WS_CHILD | WS_VISIBLE |
            SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, NULL, g_instance, NULL);

        const wchar_t *tool_labels[] = {L"Select", L"Rectangle", L"Click", L"Hand"};
        const wchar_t *tool_tips[] = {L"Select", L"Rectangle", L"Click point", L"Hand (pan)"};
        const int tool_ids[] = {ID_TOOL_SELECT, ID_TOOL_RECTANGLE, ID_TOOL_CLICK, ID_TOOL_HAND};
        for (int i = 0; i < 4; ++i) {
            DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
            if (!i) style |= WS_GROUP;
            g_tool_buttons[i] = CreateWindowW(L"BUTTON", tool_labels[i], style,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)tool_ids[i], g_instance, NULL);
            if (g_tool_buttons[i])
                SetWindowSubclass(g_tool_buttons[i], ToolButtonProc,
                                  (UINT_PTR)(i + 1), (DWORD_PTR)i);
        }
        g_tool_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hwnd, NULL, g_instance, NULL);
        if (g_tool_tooltip) {
            SetWindowPos(g_tool_tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g_tool_tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
            for (int i = 0; i < 4; ++i) {
                TOOLINFOW *tool = &g_tool_button_tooltips[i];
                ZeroMemory(tool, sizeof(*tool));
                tool->cbSize = TTTOOLINFO_V1_SIZE;
                tool->uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool->hwnd = hwnd;
                tool->uId = (UINT_PTR)g_tool_buttons[i];
                tool->hinst = g_instance;
                tool->lpszText = (wchar_t *)tool_tips[i];
                SendMessageW(g_tool_tooltip, TTM_ADDTOOLW, 0, (LPARAM)tool);
            }
        }
        const wchar_t *view_labels[] = {L"Fit", L"−", L"+"};
        const int view_ids[] = {ID_FIT, ID_ZOOM_OUT, ID_ZOOM_IN};
        for (int i = 0; i < GOLDEN_VIEW_BUTTON_COUNT; ++i)
            g_view_buttons[i] = CreateWindowW(L"BUTTON", view_labels[i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
                (HMENU)(INT_PTR)view_ids[i], g_instance, NULL);
        const wchar_t *window_labels[] = {L"Capture", L"Recapture"};
        const int window_ids[] = {ID_CAPTURE, ID_RECAPTURE};
        for (int i = 0; i < 2; ++i)
            g_window_buttons[i] = CreateWindowW(L"BUTTON", window_labels[i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
                (HMENU)(INT_PTR)window_ids[i], g_instance, NULL);
        g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_EDITLABELS, 0, 0, 0, 0, hwnd,
            (HMENU)ID_TREE, g_instance, NULL);
        g_editor = CreateWindowExW(WS_EX_CLIENTEDGE, L"GoldensEditor", NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
            (HMENU)ID_EDITOR, g_instance, NULL);
        g_editor_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_NOANIMATE | TTS_NOFADE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, hwnd, NULL, g_instance, NULL);
        if (g_editor_tooltip) {
            SetWindowPos(g_editor_tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g_editor_tooltip, TTM_SETMAXTIPWIDTH, 0,
                         golden_scale_ui(360, GetDpiForWindow(hwnd)));
            if (!golden_tooltip_register_tracking(g_editor_tooltip, g_editor,
                    g_instance, g_tooltip_text, &g_editor_tooltip_tool)) {
                DestroyWindow(g_editor_tooltip);
                g_editor_tooltip = NULL;
            }
        }
        g_windows = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd,
            (HMENU)ID_WINDOWS, g_instance, NULL);
        g_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0, hwnd, NULL, g_instance, NULL);
        g_left_splitter = CreateWindowW(L"GoldensSplitter", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_SPLITTER_LEFT, g_instance, NULL);
        g_right_splitter = CreateWindowW(L"GoldensSplitter", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_SPLITTER_RIGHT, g_instance, NULL);
        set_tool(TOOL_SELECT);
        update_context_label();
        if (g_root[0]) {
            refresh_resources();
        }
        update_status();
        refresh_windows();
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
        if (wp == WINDOW_TIMER) { refresh_windows(); refresh_preview_metadata(); }
        else if (wp == PREVIEW_TIMER) request_preview_frame();
        return 0;
    case WM_COMMAND:
        handle_command(LOWORD(wp));
        return 0;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        if (item && item->CtlID >= ID_TOOL_SELECT && item->CtlID <= ID_TOOL_HAND) {
            draw_tool_button(item);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_context_label) {
            SetBkMode((HDC)wp, TRANSPARENT);
            SetTextColor((HDC)wp, g_preview_mode ? RGB(190, 90, 0) :
                         g_resource_visible && g_image_path[0] ? RGB(0, 105, 145) :
                         GetSysColor(COLOR_BTNTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_PREVIEW_READY: {
        PreviewResult *result = (PreviewResult *)lp;
        LONG generation = result ? result->generation : (LONG)wp;
        if (generation == g_preview_generation && g_preview_mode &&
            (!result || result->target == g_preview_target)) {
            if (result && result->image.pixels) {
                golden_image_free(&g_preview_image);
                g_preview_image = result->image;
                ZeroMemory(&result->image, sizeof(result->image));
            }
            g_preview_loading = FALSE;
            InvalidateRect(g_editor, NULL, FALSE);
        }
        if (result) {
            golden_image_free(&result->image);
            free(result);
        }
        return 0;
    }
    case WM_RESOURCE_RENAMED: {
        refresh_resources();
        if (g_pending_resource_selection[0]) {
            HTREEITEM item = find_resource_item(TreeView_GetRoot(g_tree),
                                                g_pending_resource_selection);
            g_rebuilding_resources = TRUE;
            if (item) {
                TreeView_EnsureVisible(g_tree, item);
                TreeView_SelectItem(g_tree, item);
            }
            g_rebuilding_resources = FALSE;
            g_pending_resource_selection[0] = 0;
        }
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
                node->annotation_index >= 0 && node->annotation_index < g_annotation_count) {
                wchar_t name[128];
                wcsncpy(name, edit->item.pszText, _countof(name) - 1);
                name[_countof(name) - 1] = 0;
                trim_text(name);
                if (!name[0]) { show_error(L"The annotation name cannot be empty."); return FALSE; }
                if (annotation_name_exists(name, node->annotation_index)) {
                    show_error(L"That annotation name is already used in this image.");
                    return FALSE;
                }
                if (wcscmp(name, g_annotations[node->annotation_index].name)) {
                    g_selected = node->annotation_index;
                    push_undo();
                    wcscpy(g_annotations[node->annotation_index].name, name);
                    g_dirty = TRUE;
                    update_tool_availability();
                    InvalidateRect(g_editor, NULL, FALSE);
                    PostMessageW(g_main, WM_ANNOTATION_RENAMED, 0, 0);
                }
                return TRUE;
            }
            return FALSE;
        }
        if (header->idFrom == ID_TREE && header->code == TVN_SELCHANGEDW &&
            !g_rebuilding_resources) {
            NMTREEVIEWW *change = (NMTREEVIEWW *)lp;
            ResourceTreeNode *node = (ResourceTreeNode *)change->itemNew.lParam;
            if (node) activate_resource_node(node);
            else {
                BOOL was_showing_resource = !g_preview_mode && g_resource_visible;
                g_resource_visible = FALSE;
                g_selected = -1;
                update_tool_availability();
                if (was_showing_resource) {
                    HWND target = selected_capture_window();
                    if (target) preview_window(target);
                    else {
                        update_context_label();
                        InvalidateRect(g_editor, NULL, FALSE);
                    }
                }
            }
        }
        if (header->idFrom == ID_WINDOWS && header->code == TVN_SELCHANGEDW && !g_rebuilding_windows) {
            HWND target = selected_capture_window();
            if (target) preview_window(target);
            else if (g_preview_mode) {
                ResourceTreeNode *resource = selected_active_resource_node();
                if (resource) activate_resource_node(resource);
                else {
                    g_resource_visible = FALSE;
                    clear_preview();
                    InvalidateRect(g_editor, NULL, FALSE);
                }
            }
        }
        if (header->idFrom == ID_WINDOWS && header->code == NM_CLICK) {
            HWND target = clicked_capture_window();
            if (target && (!g_preview_mode || target != g_preview_target))
                preview_window(target);
        }
        if (header->idFrom == ID_TREE && header->code == NM_CLICK) {
            ResourceTreeNode *node = clicked_resource_node();
            BOOL current_png = node && node->kind == RESOURCE_PNG && node->path &&
                               !_wcsicmp(node->path, g_image_path);
            BOOL current_annotation = node && node->kind == RESOURCE_ANNOTATION &&
                node->annotation_index >= 0 && node->annotation_index < g_annotation_count;
            if (current_png || current_annotation) activate_resource_node(node);
        }
        if (header->code == NM_CLICK &&
            (header->idFrom == ID_TREE || header->idFrom == ID_WINDOWS))
            clear_tree_selection_on_blank_click(header->hwndFrom);
        if (header->idFrom == ID_TREE && header->code == NM_DBLCLK) {
            DWORD position = GetMessagePos();
            TVHITTESTINFO hit = {0};
            hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
            ScreenToClient(g_tree, &hit.pt);
            TreeView_HitTest(g_tree, &hit);
            if (hit.hItem && (hit.flags & TVHT_ONITEMLABEL))
                PostMessageW(g_main, WM_BEGIN_TREE_RENAME, 0, (LPARAM)hit.hItem);
        }
        return 0;
    }
    case WM_CLOSE:
        if (maybe_save()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, WINDOW_TIMER);
        KillTimer(hwnd, PREVIEW_TIMER);
        if (g_editor_tooltip) {
            DestroyWindow(g_editor_tooltip);
            g_editor_tooltip = NULL;
        }
        if (g_tool_tooltip) {
            DestroyWindow(g_tool_tooltip);
            g_tool_tooltip = NULL;
        }
        free_tree_item(g_tree, TreeView_GetRoot(g_tree));
        clear_preview();
        clear_image();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    g_instance = instance;
    initialize_startup_root();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&g_wic))) {
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
    g_main = CreateWindowExW(0, L"GoldensMain", APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 800, NULL, NULL, instance, NULL);
    if (!g_main) {
        IWICImagingFactory_Release(g_wic);
        CoUninitialize();
        return 1;
    }
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
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
        if (!TranslateAcceleratorW(g_main, accelerators, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(accelerators);
    IWICImagingFactory_Release(g_wic);
    CoUninitialize();
    return (int)msg.wParam;
}
