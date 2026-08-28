#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
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
#include "image_io.h"
#include "clipboard_image.h"
#include "capture_bundle.h"
#include "editor_render.h"
#include "ui_layout.h"
#include "ui_tooltip.h"
#include "ui_tool_icon.h"
#include "resource_tree.h"
#include "resource_ops.h"
#include "resource_watcher.h"
#include "atomic_file.h"
#include "history.h"
#include "resource.h"

#define APP_NAME L"Goldens"
#define EDITOR_TOOLTIP_TIMER 3
#define EDITOR_TOOLTIP_DELAY_MS 450
#define RESOURCE_TREE_TIMER 4
#define RESOURCE_TREE_COALESCE_MS 180
#define RESOURCE_TREE_RETRY_MS 1000
#define WM_RESOURCES_CHANGED (WM_APP + 2)
#define WM_ANNOTATION_RENAMED (WM_APP + 3)
#define WM_BEGIN_TREE_RENAME (WM_APP + 4)
#define WM_RESOURCE_TREE_CHANGED (WM_APP + 5)
#define CAPTURE_HOTKEY_ID 0x6a01
#define CAPTURE_HOTKEY_MODIFIERS MOD_NOREPEAT
#define CAPTURE_HOTKEY_KEY VK_F8

enum {
    ID_OPEN = 100, ID_NEW_FOLDER, ID_SAVE, ID_EXIT, ID_UNDO, ID_REDO,
    ID_COPY, ID_PASTE, ID_RENAME, ID_DELETE,
    ID_CLEAR_CLICK, ID_FIT, ID_ZOOM_OUT, ID_ZOOM_IN, ID_ACTUAL,
    ID_CAPTURE_LISTEN,
    ID_TOOL_SELECT, ID_TOOL_RECTANGLE, ID_TOOL_CLICK,
    ID_TREE = 200, ID_EDITOR, ID_SPLITTER_LEFT,
    ID_PROMPT_EDIT = 300, ID_PROMPT_OK, ID_PROMPT_CANCEL
};

typedef enum {
    TOOL_SELECT,
    TOOL_RECTANGLE,
    TOOL_CLICK
} ToolMode;

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
    HWND main, tree, editor, status;
    HWND editor_tooltip, tool_tooltip;
    HWND left_splitter;
    HWND context_label;
    HWND tool_buttons[GOLDEN_TOOL_BUTTON_COUNT];
    HWND view_buttons[GOLDEN_VIEW_BUTTON_COUNT];
    HMENU file_menu, edit_menu, view_menu, capture_menu;
    TOOLINFOW tool_button_tooltips[GOLDEN_TOOL_BUTTON_COUNT];
    IWICImagingFactory *wic;

    wchar_t root[MAX_PATH * 4];
    wchar_t image_path[MAX_PATH * 4];
    wchar_t current_dir[MAX_PATH * 4];
    DWORD image_volume_serial;
    DWORD image_file_index_high;
    DWORD image_file_index_low;
    BOOL image_identity_valid;
    BYTE *pixels;
    UINT image_w, image_h, stride;
    uint64_t image_revision;
    GoldenBackBuffer editor_buffer;
    GoldenImageCache image_cache;

    BOOL resource_visible;

    BOOL capture_hotkey_enabled;
    BOOL capture_hotkey_registered;
    BOOL capture_in_progress;

    double zoom;
    int pan_x, pan_y;
    BOOL panning;
    POINT pan_start;
    int pan_origin_x, pan_origin_y;

    Annotation annotations[MAX_ANNOTATIONS];
    int annotation_count;
    int selected;
    BOOL dirty;
    Annotation saved_annotations[MAX_ANNOTATIONS];
    int saved_annotation_count;
    GoldenHistory history;
    int drag_mode;
    POINT drag_start;
    RECT drag_original;
    BOOL drawing;
    POINT draw_start, draw_current;
    ToolMode tool;

    BOOL rebuilding_resources;
    wchar_t pending_resource_selection[MAX_PATH * 4];
    GoldenResourceWatcher resource_watcher;
    BOOL resource_watcher_needs_restart;
    BOOL resource_refresh_pending;
    BOOL resource_dragging;
    HTREEITEM resource_drag_source;
    HTREEITEM resource_drop_target;

    int left_column_width;
    BOOL left_collapsed;
    BOOL splitter_dragging;
    POINT splitter_drag_start;
    int splitter_width_start;

    int tooltip_pending;
    int tooltip_visible;
    BOOL editor_mouse_tracking;
    wchar_t tooltip_text[128];
    wchar_t click_tooltip_text[64];
    TOOLINFOW editor_tooltip_tool;
    int hovered_tool;
} GoldenAppState;

static GoldenAppState g;

static void initialize_app_state(HINSTANCE instance) {
    g.instance = instance;
    g.selected = -1;
    g.tool = TOOL_SELECT;
    g.left_column_width = GOLDEN_RESOURCE_PANE_DEFAULT;
    g.capture_hotkey_enabled = TRUE;
    g.tooltip_pending = -1;
    g.tooltip_visible = -1;
    g.hovered_tool = -1;
    wcscpy(g.click_tooltip_text, L"Place or move click point");
}

static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK EditorProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK PromptProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SplitterProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ToolButtonProc(HWND, UINT, WPARAM, LPARAM,
                                       UINT_PTR, DWORD_PTR);
static BOOL prompt_text(HWND owner, const wchar_t *title, const wchar_t *label,
                        wchar_t *value, size_t capacity);
static void refresh_annotation_tree(void);
static void update_context_label(void);
static void zoom_by(double factor, const POINT *anchor);
static void update_tool_availability(void);
static void update_menu_availability(void);
static void set_tool_with_focus(ToolMode tool, BOOL focus_editor);
static void set_tool(ToolMode tool);
static void layout_children(HWND hwnd);
static void hide_annotation_tooltip(HWND hwnd);
static void update_annotation_hover(HWND hwnd, POINT client);
static void draw_tool_button(const DRAWITEMSTRUCT *item);
static void restart_resource_watcher(void);
static void schedule_resource_refresh(UINT delay_ms);
static void cancel_resource_drag(void);
static void create_folder(void);
static BOOL apply_resource_history(GoldenHistoryEntry *entry, BOOL undo);
static void discard_history_entry(GoldenHistoryEntry *entry, void *context);
static BOOL make_history_temporary_path(const wchar_t *extension,
                                        wchar_t *path, size_t capacity);
static void update_state_after_resource_move(GoldenHistoryKind kind,
                                             const wchar_t *source,
                                             const wchar_t *destination);
static void capture_foreground_bundle(void);
static void set_capture_hotkey_enabled(BOOL enabled, BOOL remember);

static void show_error(const wchar_t *message) {
    MessageBoxW(g.main, message, APP_NAME, MB_OK | MB_ICONERROR);
}

static void delete_resource_pair(const wchar_t *png_path) {
    if (!png_path || !png_path[0]) return;
    wchar_t json[MAX_PATH * 4];
    golden_resource_json_path(png_path, json, _countof(json));
    DeleteFileW(png_path);
    DeleteFileW(json);
}

static void cleanup_staged_directory(const wchar_t *path, BOOL delete_tree) {
    if (!path || !path[0]) return;
    if (delete_tree) golden_delete_directory_tree(path);
    wchar_t parent[MAX_PATH * 4];
    if (golden_path_copy(path, parent, _countof(parent)) &&
        PathRemoveFileSpecW(parent)) RemoveDirectoryW(parent);
}

static void discard_history_entry(GoldenHistoryEntry *entry, void *context) {
    UNREFERENCED_PARAMETER(context);
    if (entry->kind == GOLDEN_HISTORY_CREATE_PNG && entry->staged)
        delete_resource_pair(entry->destination);
    else if (entry->kind == GOLDEN_HISTORY_DELETE_PNG && entry->staged)
        delete_resource_pair(entry->destination);
    else if (entry->kind == GOLDEN_HISTORY_DELETE_DIRECTORY) {
        cleanup_staged_directory(entry->destination, entry->staged);
    }
    else if (entry->kind == GOLDEN_HISTORY_REPLACE_MOVE_PNG && entry->staged)
        delete_resource_pair(entry->auxiliary);
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

static BOOL read_file_identity(const wchar_t *path, DWORD *volume_serial,
                               DWORD *file_index_high, DWORD *file_index_low) {
    HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    BY_HANDLE_FILE_INFORMATION info;
    BOOL ok = GetFileInformationByHandle(file, &info);
    CloseHandle(file);
    if (!ok) return FALSE;
    *volume_serial = info.dwVolumeSerialNumber;
    *file_index_high = info.nFileIndexHigh;
    *file_index_low = info.nFileIndexLow;
    return TRUE;
}

static void remember_image_identity(const wchar_t *path) {
    g.image_identity_valid = read_file_identity(
        path, &g.image_volume_serial, &g.image_file_index_high,
        &g.image_file_index_low);
}

static BOOL replace_path_prefix(wchar_t *path, size_t capacity,
                                const wchar_t *old_prefix,
                                const wchar_t *new_prefix) {
    if (!path[0] || !golden_path_is_same_or_inside(path, old_prefix)) return FALSE;
    size_t old_length = wcslen(old_prefix);
    while (old_length && (old_prefix[old_length - 1] == L'\\' ||
                          old_prefix[old_length - 1] == L'/')) --old_length;
    wchar_t changed[MAX_PATH * 4];
    int written = _snwprintf(changed, _countof(changed), L"%s%s",
                             new_prefix, path + old_length);
    if (written < 0 || (size_t)written >= capacity) return FALSE;
    wcscpy(path, changed);
    return TRUE;
}

static void update_status(void) {
    if (!g.status) return;
    wchar_t text[MAX_PATH * 4 + 96];
    const wchar_t *folder = g.current_dir[0] ? g.current_dir : g.root;
    _snwprintf(text, _countof(text), L"  Capture folder: %s  •  F8 capture: %s",
               folder[0] ? folder : L"(unavailable)",
               g.capture_hotkey_enabled ? L"On" : L"Off");
    SetWindowTextW(g.status, text);
}

static void update_context_label(void) {
    if (!g.context_label) return;
    wchar_t text[MAX_PATH * 4 + 64];
    if (g.resource_visible && g.image_path[0])
        _snwprintf(text, _countof(text), L"  Editing resource  —  %s",
                   PathFindFileNameW(g.image_path));
    else
        wcscpy(text, L"  No resource selected");
    SetWindowTextW(g.context_label, text);
}

static void update_dirty_state(void) {
    g.dirty = !golden_history_annotations_equal(
        g.annotations, g.annotation_count,
        g.saved_annotations, g.saved_annotation_count);
    update_menu_availability();
}

static void mark_current_annotations_saved(void) {
    g.saved_annotation_count = g.annotation_count;
    memcpy(g.saved_annotations, g.annotations,
           sizeof(Annotation) * (size_t)g.annotation_count);
    update_dirty_state();
}

static BOOL snapshot_current(GoldenHistoryEntry *entry) {
    return golden_history_entry_annotations(entry, g.annotations,
                                            g.annotation_count, g.selected);
}

static void restore_snapshot(const GoldenHistoryEntry *entry) {
    g.annotation_count = entry->annotation_count;
    g.selected = entry->selected_annotation;
    memcpy(g.annotations, entry->annotations,
           sizeof(Annotation) * (size_t)entry->annotation_count);
    update_dirty_state();
    update_tool_availability();
    refresh_annotation_tree();
    InvalidateRect(g.editor, NULL, FALSE);
}

static BOOL push_undo(void) {
    GoldenHistoryEntry entry = {0};
    if (!snapshot_current(&entry)) {
        show_error(L"Goldens could not preserve this edit for undo.");
        return FALSE;
    }
    golden_history_push_new(&g.history, &entry);
    return TRUE;
}

static void push_resource_undo(GoldenHistoryKind kind, const wchar_t *source,
                               const wchar_t *destination) {
    GoldenHistoryEntry entry;
    golden_history_entry_resource(&entry, kind, source, destination, NULL);
    golden_history_push_new(&g.history, &entry);
}

static void undo_action(void) {
    GoldenHistoryEntry entry = {0};
    if (!golden_history_pop_undo(&g.history, &entry)) return;
    GoldenHistoryEntry redo = {0};
    if (entry.kind == GOLDEN_HISTORY_ANNOTATIONS) {
        if (!snapshot_current(&redo)) {
            golden_history_restore_undo(&g.history, &entry);
            return;
        }
        restore_snapshot(&entry);
        golden_history_entry_dispose(&entry);
        golden_history_transfer_to_redo(&g.history, &redo);
    } else if (!apply_resource_history(&entry, TRUE)) {
        golden_history_restore_undo(&g.history, &entry);
        return;
    } else {
        golden_history_transfer_to_redo(&g.history, &entry);
    }
    update_menu_availability();
}

static void redo_action(void) {
    GoldenHistoryEntry entry = {0};
    if (!golden_history_pop_redo(&g.history, &entry)) return;
    GoldenHistoryEntry undo = {0};
    if (entry.kind == GOLDEN_HISTORY_ANNOTATIONS) {
        if (!snapshot_current(&undo)) {
            golden_history_restore_redo(&g.history, &entry);
            return;
        }
        restore_snapshot(&entry);
        golden_history_entry_dispose(&entry);
        golden_history_transfer_to_undo(&g.history, &undo);
    } else if (!apply_resource_history(&entry, FALSE)) {
        golden_history_restore_redo(&g.history, &entry);
        return;
    } else {
        golden_history_transfer_to_undo(&g.history, &entry);
    }
    update_menu_availability();
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
    g.image_identity_valid = FALSE;
    ++g.image_revision;
}

static BYTE *active_pixels(void) {
    return g.resource_visible ? g.pixels : NULL;
}
static UINT active_width(void) {
    return g.resource_visible ? g.image_w : 0;
}
static UINT active_height(void) {
    return g.resource_visible ? g.image_h : 0;
}
static UINT active_stride(void) {
    return g.resource_visible ? g.stride : 0;
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
    golden_history_remove_annotations(&g.history);
    wchar_t path[MAX_PATH * 4];
    if (!json_path_for(png_path, path, _countof(path))) {
        show_error(L"The resource path is too long to locate its annotation JSON file.");
        return;
    }
    FILE *file = _wfopen(path, L"rb");
    if (file) {
        if (fseek(file, 0, SEEK_END) == 0) {
            errno = 0;
            long length = ftell(file);
            if (length > 0 && length <= 16 * 1024 * 1024 && errno == 0 &&
                fseek(file, 0, SEEK_SET) == 0) {
                char *text = (char *)malloc((size_t)length + 1);
                if (text) {
                    size_t got = fread(text, 1, (size_t)length, file);
                    text[got] = 0;
                    int count = MAX_ANNOTATIONS;
                    if (golden_document_parse_utf8(text, got,
                                                   g.annotations, &count))
                        g.annotation_count = count;
                    else
                        show_error(L"The annotation JSON is invalid and was not loaded.");
                    free(text);
                }
            }
        }
        fclose(file);
    }
    mark_current_annotations_saved();
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
    if (ok) mark_current_annotations_saved();
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

static HTREEITEM directory_item_for(HTREEITEM item) {
    ResourceTreeNode *node = tree_node_data(item);
    if (!node) return NULL;
    if (node->kind == RESOURCE_DIRECTORY) return item;
    if (node->kind == RESOURCE_ANNOTATION)
        item = TreeView_GetParent(g.tree, item);
    return item ? TreeView_GetParent(g.tree, item) : NULL;
}

static const wchar_t *selected_directory_path(void) {
    return golden_resource_tree_selected_directory(g.tree);
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

typedef struct {
    wchar_t **expanded_paths;
    size_t expanded_count;
    size_t expanded_capacity;
    BOOL had_selection;
    ResourceNodeKind selected_kind;
    wchar_t selected_path[MAX_PATH * 4];
    int selected_annotation;
} ResourceTreeSnapshot;

static void capture_expanded_resources(HTREEITEM item,
                                       ResourceTreeSnapshot *snapshot) {
    while (item) {
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->kind == RESOURCE_DIRECTORY && node->path &&
            (TreeView_GetItemState(g.tree, item, TVIS_EXPANDED) & TVIS_EXPANDED)) {
            if (snapshot->expanded_count == snapshot->expanded_capacity) {
                size_t next = snapshot->expanded_capacity > SIZE_MAX / 2 ? 0 :
                    snapshot->expanded_capacity ?
                    snapshot->expanded_capacity * 2 : 16;
                if (next &&
                    next <= SIZE_MAX / sizeof(*snapshot->expanded_paths)) {
                    wchar_t **grown = (wchar_t **)realloc(
                        snapshot->expanded_paths,
                        next * sizeof(*snapshot->expanded_paths));
                    if (grown) {
                        snapshot->expanded_paths = grown;
                        snapshot->expanded_capacity = next;
                    }
                }
            }
            if (snapshot->expanded_count < snapshot->expanded_capacity) {
                wchar_t *path = dup_wide(node->path);
                if (path)
                    snapshot->expanded_paths[snapshot->expanded_count++] = path;
            }
        }
        capture_expanded_resources(TreeView_GetChild(g.tree, item), snapshot);
        item = TreeView_GetNextSibling(g.tree, item);
    }
}

static void capture_resource_tree_snapshot(ResourceTreeSnapshot *snapshot) {
    ZeroMemory(snapshot, sizeof(*snapshot));
    snapshot->selected_annotation = -1;
    HTREEITEM selected = TreeView_GetSelection(g.tree);
    ResourceTreeNode *node = tree_node_data(selected);
    if (node) {
        snapshot->had_selection = TRUE;
        snapshot->selected_kind = node->kind;
        snapshot->selected_annotation = node->annotation_index;
        if (node->path)
            golden_path_copy(node->path, snapshot->selected_path,
                             _countof(snapshot->selected_path));
        else {
            ResourceTreeNode *parent = tree_node_data(
                TreeView_GetParent(g.tree, selected));
            if (parent && parent->path)
                golden_path_copy(parent->path, snapshot->selected_path,
                                 _countof(snapshot->selected_path));
        }
    }
    capture_expanded_resources(TreeView_GetRoot(g.tree), snapshot);
}

static void release_resource_tree_snapshot(ResourceTreeSnapshot *snapshot) {
    for (size_t i = 0; i < snapshot->expanded_count; ++i)
        free(snapshot->expanded_paths[i]);
    free(snapshot->expanded_paths);
}

static HTREEITEM find_resource_with_current_identity(HTREEITEM item) {
    while (item) {
        ResourceTreeNode *node = tree_node_data(item);
        if (node && node->kind == RESOURCE_PNG && node->path) {
            DWORD volume = 0, high = 0, low = 0;
            if (read_file_identity(node->path, &volume, &high, &low) &&
                volume == g.image_volume_serial &&
                high == g.image_file_index_high && low == g.image_file_index_low)
                return item;
        }
        HTREEITEM found = find_resource_with_current_identity(
            TreeView_GetChild(g.tree, item));
        if (found) return found;
        item = TreeView_GetNextSibling(g.tree, item);
    }
    return NULL;
}

static void reconcile_current_resource(ResourceTreeSnapshot *snapshot) {
    if (!g.image_path[0]) return;
    wchar_t old_path[MAX_PATH * 4];
    if (!golden_path_copy(g.image_path, old_path, _countof(old_path))) return;
    HTREEITEM current = find_resource_item(TreeView_GetRoot(g.tree), g.image_path);
    if (current) {
        remember_image_identity(g.image_path);
        return;
    }
    if (g.image_identity_valid)
        current = find_resource_with_current_identity(TreeView_GetRoot(g.tree));
    ResourceTreeNode *moved = tree_node_data(current);
    if (moved && moved->path) {
        golden_path_copy(moved->path, g.image_path, _countof(g.image_path));
        parent_dir_for(g.image_path, g.current_dir, _countof(g.current_dir));
        if (snapshot->had_selection &&
            !_wcsicmp(snapshot->selected_path, old_path))
            golden_path_copy(g.image_path, snapshot->selected_path,
                             _countof(snapshot->selected_path));
        remember_image_identity(g.image_path);
        update_context_label();
        wchar_t title[MAX_PATH * 4 + 32];
        _snwprintf(title, _countof(title), L"Goldens — %s",
                   PathFindFileNameW(g.image_path));
        SetWindowTextW(g.main, title);
        update_status();
    } else if (!g.dirty) {
        clear_image();
        g.image_path[0] = 0;
        g.annotation_count = g.saved_annotation_count = 0;
        g.selected = -1;
        g.dirty = FALSE;
        golden_history_clear(&g.history);
        SetWindowTextW(g.main, APP_NAME);
        update_context_label();
        InvalidateRect(g.editor, NULL, FALSE);
    }
    DWORD current_attributes = GetFileAttributesW(g.current_dir);
    if (current_attributes == INVALID_FILE_ATTRIBUTES ||
        !(current_attributes & FILE_ATTRIBUTE_DIRECTORY))
        golden_path_copy(g.root, g.current_dir, _countof(g.current_dir));
}

static void restore_resource_tree_snapshot(
    const ResourceTreeSnapshot *snapshot) {
    for (size_t i = 0; i < snapshot->expanded_count; ++i) {
        HTREEITEM item = find_resource_item(TreeView_GetRoot(g.tree),
                                            snapshot->expanded_paths[i]);
        if (item) TreeView_Expand(g.tree, item, TVE_EXPAND);
    }
    if (!snapshot->had_selection || !snapshot->selected_path[0]) return;
    HTREEITEM target = NULL;
    if (snapshot->selected_kind == RESOURCE_ANNOTATION &&
        !_wcsicmp(snapshot->selected_path, g.image_path) &&
        snapshot->selected_annotation >= 0)
        target = find_annotation_item(snapshot->selected_annotation);
    else
        target = find_resource_item(TreeView_GetRoot(g.tree),
                                    snapshot->selected_path);
    if (target) {
        TreeView_EnsureVisible(g.tree, target);
        TreeView_SelectItem(g.tree, target);
    }
}

static void refresh_resources_expanding(const wchar_t *expand_path) {
    ResourceTreeSnapshot snapshot;
    capture_resource_tree_snapshot(&snapshot);
    g.rebuilding_resources = TRUE;
    free_tree_item(g.tree, TreeView_GetRoot(g.tree));
    TreeView_DeleteAllItems(g.tree);
    if (g.root[0]) {
        wchar_t journal[MAX_PATH * 4];
        if (!golden_path_join(g.root, L".goldens-move-journal",
                              journal, _countof(journal))) {
            show_error(L"The resource root path is too long for transaction recovery.");
            g.rebuilding_resources = FALSE;
            release_resource_tree_snapshot(&snapshot);
            update_tool_availability();
            return;
        }
        if (golden_recover_resource_pair_move(journal) != GOLDEN_RENAME_OK) {
            show_error(L"Goldens found an interrupted resource move that Windows could not recover. Close programs using the files, then refresh.");
            g.rebuilding_resources = FALSE;
            release_resource_tree_snapshot(&snapshot);
            update_tool_availability();
            return;
        }
        const wchar_t *label = PathFindFileNameW(g.root);
        if (!*label) label = g.root;
        HTREEITEM root = insert_path_item(g.tree, TVI_ROOT, label, g.root, TRUE);
        populate_directory(root, g.root);
        TreeView_Expand(g.tree, root, TVE_EXPAND);
    }
    g.rebuilding_resources = FALSE;
    reconcile_current_resource(&snapshot);
    refresh_annotation_tree();
    g.rebuilding_resources = TRUE;
    restore_resource_tree_snapshot(&snapshot);
    if (expand_path && expand_path[0]) {
        HTREEITEM target = find_resource_item(TreeView_GetRoot(g.tree),
                                              expand_path);
        if (target) {
            TreeView_EnsureVisible(g.tree, target);
            TreeView_Expand(g.tree, target, TVE_EXPAND);
        }
    }
    g.rebuilding_resources = FALSE;
    release_resource_tree_snapshot(&snapshot);
    update_tool_availability();
    update_tool_availability();
}

static void restart_resource_watcher(void) {
    KillTimer(g.main, RESOURCE_TREE_TIMER);
    g.resource_refresh_pending = FALSE;
    golden_resource_watcher_stop(&g.resource_watcher, 2000);
    g.resource_watcher_needs_restart = FALSE;
    if (g.root[0] && !golden_resource_watcher_start(
            &g.resource_watcher, g.root, g.main, WM_RESOURCE_TREE_CHANGED)) {
        g.resource_watcher_needs_restart = TRUE;
        schedule_resource_refresh(RESOURCE_TREE_RETRY_MS);
    }
}

static void schedule_resource_refresh(UINT delay_ms) {
    if (g.resource_refresh_pending) return;
    g.resource_refresh_pending = TRUE;
    if (!SetTimer(g.main, RESOURCE_TREE_TIMER, delay_ms, NULL))
        PostMessageW(g.main, WM_TIMER, RESOURCE_TREE_TIMER, 0);
}

static void refresh_resources(void) {
    refresh_resources_expanding(NULL);
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

static void remember_capture_hotkey_setting(void) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Goldens", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        DWORD enabled = g.capture_hotkey_enabled ? 1u : 0u;
        RegSetValueExW(key, L"CaptureHotkeyEnabled", 0, REG_DWORD,
                       (const BYTE *)&enabled, sizeof(enabled));
        RegCloseKey(key);
    }
}

static void load_capture_hotkey_setting(void) {
    HKEY key;
    DWORD enabled = 1, type = 0, bytes = sizeof(enabled);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Goldens", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return;
    if (RegQueryValueExW(key, L"CaptureHotkeyEnabled", NULL, &type,
                         (BYTE *)&enabled, &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(enabled))
        g.capture_hotkey_enabled = enabled != 0;
    RegCloseKey(key);
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
        g.image_path[0] = 0;
        g.selected = -1;
        g.annotation_count = g.saved_annotation_count = 0;
        g.dirty = FALSE;
        golden_history_clear(&g.history);
        update_tool_availability();
        remember_root();
        restart_resource_watcher();
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
    g.resource_visible = TRUE;
    g.zoom = 0.0;
    g.pan_x = g.pan_y = 0;
    golden_path_copy(next_path, g.image_path, _countof(g.image_path));
    golden_path_copy(next_directory, g.current_dir, _countof(g.current_dir));
    remember_image_identity(next_path);
    update_status();
    load_annotations(next_path);
    update_tool_availability();
    refresh_annotation_tree();
    update_tool_availability();
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
    if (!active_pixels() || !width || !height) {
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
    if (!g.panning && !g.drag_mode && !g.drawing &&
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
    if (hit != g.tooltip_pending || g.panning ||
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
        const wchar_t *message = g.root[0] ?
            L"Select a PNG from the resource tree" :
            L"Open a resource folder to begin";
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
    for (int i = 0; i < g.annotation_count; ++i) {
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
            golden_draw_click_mark(dc, (POINT){cx, cy});
        }
    }
    if (g.drawing) {
        RECT boundary = {min(g.draw_start.x, g.draw_current.x), min(g.draw_start.y, g.draw_current.y),
                         max(g.draw_start.x, g.draw_current.x), max(g.draw_start.y, g.draw_current.y)};
        RECT r = annotation_screen_rect_for_layout(&dest, scale, &boundary);
        golden_fill_tinted_rect(dc, &r, RGB(255, 150, 0), 112);
        golden_draw_boundary(dc, &r, RGB(255, 210, 0), 3, PS_SOLID);
    }
    wchar_t info_text[512];
    _snwprintf(info_text, _countof(info_text),
        L"%u × %u px  •  %d annotation%s%s  •  %.0f%%",
        image_w, image_h, g.annotation_count,
        g.annotation_count == 1 ? L"" : L"s",
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
    if (!push_undo()) return;
    wcscpy(g.annotations[g.selected].name, name);
    update_dirty_state();
    refresh_annotation_tree();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void delete_selected(void) {
    if (g.selected < 0) return;
    if (!push_undo()) return;
    memmove(&g.annotations[g.selected], &g.annotations[g.selected + 1],
            sizeof(Annotation) *
                (size_t)(g.annotation_count - g.selected - 1));
    g.annotation_count--;
    if (g.selected >= g.annotation_count) g.selected = g.annotation_count - 1;
    update_dirty_state();
    update_tool_availability();
    refresh_annotation_tree();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void clear_click(void) {
    if (g.selected < 0 || !g.annotations[g.selected].has_click) return;
    if (!push_undo()) return;
    g.annotations[g.selected].has_click = FALSE;
    update_dirty_state();
    update_tool_availability();
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
                     g.tool == TOOL_SELECT ? IDC_ARROW :
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
        POINT client = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, image;
        if (!client_to_image(hwnd, client, &image)) {
            if (g.tool == TOOL_SELECT) deselect_annotation();
            return 0;
        }
        if (g.tool == TOOL_CLICK) {
            if (g.selected >= 0 && g.selected < g.annotation_count &&
                PtInRect(&g.annotations[g.selected].boundary, image)) {
                if (!push_undo()) return 0;
                golden_set_click(&g.annotations[g.selected], image);
                update_dirty_state();
                update_tool_availability();
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
                if (!push_undo()) return 0;
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
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g.drag_mode) {
            g.drag_mode = 0;
            update_dirty_state();
            ReleaseCapture();
            set_editor_cursor();
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g.drawing) {
            ReleaseCapture();
            g.drawing = FALSE;
            RECT r = golden_normalize_rect(g.draw_start, g.draw_current);
            if (r.right - r.left >= 2 && r.bottom - r.top >= 2) {
                if (!push_undo()) return 0;
                Annotation *a = &g.annotations[g.annotation_count];
                ZeroMemory(a, sizeof(*a));
                make_unique_name(a->name, _countof(a->name));
                a->boundary = r;
                g.selected = g.annotation_count++;
                update_dirty_state();
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
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT anchor = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &anchor);
        zoom_by(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1.25 : 0.8, &anchor);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (g.tool == TOOL_SELECT) rename_selected();
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

static ResourceTreeNode *selected_tree_node(void) {
    return g.tree ? tree_node_data(TreeView_GetSelection(g.tree)) : NULL;
}

static BOOL resource_delete_available(void) {
    HWND focus = GetFocus();
    ResourceTreeNode *node = selected_tree_node();
    BOOL deletable = node && node->path &&
        (node->kind == RESOURCE_PNG ||
         (node->kind == RESOURCE_DIRECTORY && _wcsicmp(node->path, g.root)));
    return deletable &&
           (focus == g.tree || IsChild(g.tree, focus));
}

static BOOL annotation_action_available(void) {
    return g.resource_visible &&
           selected_active_resource_node() != NULL &&
           g.selected >= 0 && g.selected < g.annotation_count;
}

static BOOL delete_action_available(void) {
    return resource_delete_available() || annotation_action_available();
}

static BOOL click_clear_available(void) {
    return annotation_action_available() && g.annotations[g.selected].has_click;
}

static BOOL tree_rename_available(void) {
    HWND focus = GetFocus();
    if (focus != g.tree && !IsChild(g.tree, focus))
        return annotation_action_available();
    ResourceTreeNode *node = tree_node_data(TreeView_GetSelection(g.tree));
    return node && ((node->kind == RESOURCE_DIRECTORY && node->path &&
                     _wcsicmp(node->path, g.root)) ||
                    node->kind == RESOURCE_PNG ||
                    (node->kind == RESOURCE_ANNOTATION &&
                     node->annotation_index >= 0 &&
                     node->annotation_index < g.annotation_count));
}

static void set_menu_command_enabled(HMENU menu, UINT command, BOOL enabled) {
    if (!menu) return;
    EnableMenuItem(menu, command, MF_BYCOMMAND |
                   (enabled ? MF_ENABLED : MF_GRAYED));
}

static void update_menu_availability(void) {
    DWORD root_attributes = g.root[0] ? GetFileAttributesW(g.root) :
                                       INVALID_FILE_ATTRIBUTES;
    BOOL root_available = root_attributes != INVALID_FILE_ATTRIBUTES &&
                          (root_attributes & FILE_ATTRIBUTE_DIRECTORY);
    BOOL image_available = active_pixels() && active_width() && active_height();
    const wchar_t *paste_directory = selected_directory_path();
    DWORD paste_attributes = paste_directory ?
        GetFileAttributesW(paste_directory) : INVALID_FILE_ATTRIBUTES;
    BOOL paste_directory_available =
        paste_attributes != INVALID_FILE_ATTRIBUTES &&
        (paste_attributes & FILE_ATTRIBUTE_DIRECTORY);
    set_menu_command_enabled(g.file_menu, ID_NEW_FOLDER, root_available);
    set_menu_command_enabled(g.file_menu, ID_SAVE,
                             g.dirty && g.image_path[0]);
    set_menu_command_enabled(g.edit_menu, ID_UNDO,
                             golden_history_can_undo(&g.history));
    set_menu_command_enabled(g.edit_menu, ID_REDO,
                             golden_history_can_redo(&g.history));
    set_menu_command_enabled(g.edit_menu, ID_COPY, image_available);
    set_menu_command_enabled(g.edit_menu, ID_PASTE,
                             paste_directory_available &&
                             golden_clipboard_has_image());
    set_menu_command_enabled(g.edit_menu, ID_RENAME, tree_rename_available());
    BOOL deleting_resource = resource_delete_available();
    ResourceTreeNode *delete_node = deleting_resource ? selected_tree_node() : NULL;
    ModifyMenuW(g.edit_menu, ID_DELETE, MF_BYCOMMAND | MF_STRING, ID_DELETE,
                delete_node && delete_node->kind == RESOURCE_DIRECTORY ?
                                    L"Delete Folder…\tDel" :
                deleting_resource ? L"Delete PNG and Sidecar\tDel" :
                annotation_action_available() ? L"Delete Annotation\tDel" :
                                                L"Delete\tDel");
    set_menu_command_enabled(g.edit_menu, ID_DELETE, delete_action_available());
    set_menu_command_enabled(g.edit_menu, ID_CLEAR_CLICK,
                             click_clear_available());
    set_menu_command_enabled(g.view_menu, ID_FIT, image_available);
    set_menu_command_enabled(g.view_menu, ID_ZOOM_OUT, image_available);
    set_menu_command_enabled(g.view_menu, ID_ZOOM_IN, image_available);
    set_menu_command_enabled(g.view_menu, ID_ACTUAL, image_available);
    set_menu_command_enabled(g.capture_menu, ID_CAPTURE_LISTEN,
                             !g.capture_in_progress);
    if (g.capture_menu)
        CheckMenuItem(g.capture_menu, ID_CAPTURE_LISTEN,
                      MF_BYCOMMAND | (g.capture_hotkey_enabled ?
                                      MF_CHECKED : MF_UNCHECKED));
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

static BOOL valid_resource_name(const wchar_t *name) {
    if (!name[0] || !wcscmp(name, L".") || !wcscmp(name, L"..")) return FALSE;
    size_t length = wcslen(name);
    return name[length - 1] != L'.' && !wcspbrk(name, L"<>:\"/\\|?*");
}

static void normalize_capture_name(wchar_t *name) {
    trim_text(name);
    size_t length = wcslen(name);
    if (ends_with_png(name)) name[length - 4] = 0;
}

static BOOL child_path(const wchar_t *directory, const wchar_t *name,
                       wchar_t *path, size_t capacity) {
    return golden_path_join(directory, name, path, capacity);
}

static void select_resource_after_refresh(const wchar_t *path) {
    if (!golden_path_copy(path, g.pending_resource_selection,
                          _countof(g.pending_resource_selection))) return;
    PostMessageW(g.main, WM_RESOURCES_CHANGED, 0, 0);
}

static void update_title_for_active_resource(void) {
    update_context_label();
    update_status();
    if (g.image_path[0]) {
        wchar_t title[MAX_PATH * 4 + 32];
        _snwprintf(title, _countof(title), L"Goldens — %s",
                   PathFindFileNameW(g.image_path));
        SetWindowTextW(g.main, title);
    }
}

static void clear_active_resource(const wchar_t *parent) {
    clear_image();
    g.image_path[0] = 0;
    g.annotation_count = g.saved_annotation_count = 0;
    g.selected = -1;
    g.dirty = FALSE;
    if (parent) golden_path_copy(parent, g.current_dir, _countof(g.current_dir));
    SetWindowTextW(g.main, APP_NAME);
    update_context_label();
    update_status();
    update_tool_availability();
    InvalidateRect(g.editor, NULL, FALSE);
}

static void create_folder(void) {
    const wchar_t *directory = selected_directory_path();
    if (!directory || !directory[0]) {
        show_error(L"Open a resource folder before creating a subfolder.");
        return;
    }
    wchar_t name[256] = L"New Folder";
    if (!prompt_text(g.main, L"New folder", L"Folder name:",
                     name, _countof(name))) return;
    trim_text(name);
    if (!valid_resource_name(name)) {
        show_error(L"Enter a valid Windows folder name.");
        return;
    }
    wchar_t path[MAX_PATH * 4];
    if (!child_path(directory, name, path, _countof(path))) {
        show_error(L"The folder path is too long.");
        return;
    }
    if (!CreateDirectoryW(path, NULL)) {
        show_error(GetLastError() == ERROR_ALREADY_EXISTS ?
            L"A file or folder with that name already exists here." :
            L"Windows could not create the folder.");
        return;
    }
    push_resource_undo(GOLDEN_HISTORY_CREATE_DIRECTORY, NULL, path);
    select_resource_after_refresh(path);
}

static const wchar_t *resource_pair_error(GoldenResourceRenameResult result) {
    return result == GOLDEN_RENAME_INVALID_PATH ?
        L"The resource path is invalid or too long." :
        result == GOLDEN_RENAME_PNG_EXISTS ?
        L"A PNG with that resource name already exists in the destination folder." :
        result == GOLDEN_RENAME_JSON_EXISTS ?
        L"A JSON annotation file with that resource name already exists in the destination folder." :
        result == GOLDEN_RENAME_JSON_FAILED_ROLLED_BACK ?
        L"The JSON sidecar could not be moved, so the PNG move was rolled back." :
        result == GOLDEN_RENAME_ROLLBACK_FAILED ?
        L"The JSON move and PNG rollback both failed. Check the source and destination folders before continuing." :
        result == GOLDEN_RENAME_JOURNAL_FAILED ?
        L"Goldens could not create its transaction journal, so no files were moved." :
        result == GOLDEN_RENAME_RECOVERY_FAILED ?
        L"An earlier interrupted resource move could not be recovered. Close programs using the files, then refresh." :
        L"Windows could not move the resource pair.";
}

static GoldenResourceRenameResult move_resource_pair(const wchar_t *source,
                                                      const wchar_t *destination) {
    wchar_t journal[MAX_PATH * 4];
    if (!g.root[0] || !golden_path_join(g.root, L".goldens-move-journal",
                                        journal, _countof(journal)))
        return GOLDEN_RENAME_JOURNAL_FAILED;
    return golden_rename_resource_pair_transactional(source, destination,
                                                      journal);
}

static BOOL move_two_resource_pairs(const wchar_t *first_source,
                                    const wchar_t *first_destination,
                                    const wchar_t *second_source,
                                    const wchar_t *second_destination) {
    GoldenResourceRenameResult first = move_resource_pair(first_source,
                                                           first_destination);
    if (first != GOLDEN_RENAME_OK) {
        show_error(resource_pair_error(first));
        return FALSE;
    }
    GoldenResourceRenameResult second = move_resource_pair(second_source,
                                                            second_destination);
    if (second == GOLDEN_RENAME_OK) return TRUE;
    GoldenResourceRenameResult rollback = move_resource_pair(first_destination,
                                                              first_source);
    if (rollback == GOLDEN_RENAME_OK)
        show_error(resource_pair_error(second));
    else
        show_error(L"Goldens could not finish replacing the PNG, and Windows also could not restore the original destination. Check the source and destination folders before continuing.");
    return FALSE;
}

static BOOL replace_resource_after_confirmation(const wchar_t *source,
                                                const wchar_t *destination) {
    wchar_t message[MAX_PATH * 4 + 256];
    _snwprintf(message, _countof(message),
        L"A PNG named '%s' already exists in the destination folder.\n\n"
        L"Replace it and its JSON sidecar? You can undo this with Ctrl+Z.",
        PathFindFileNameW(destination));
    if (MessageBoxW(g.main, message, APP_NAME,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return FALSE;
    wchar_t backup[MAX_PATH * 4];
    if (!make_history_temporary_path(L".png", backup, _countof(backup))) {
        show_error(L"Goldens could not reserve recoverable undo storage for the PNG being replaced.");
        return FALSE;
    }
    if (!move_two_resource_pairs(destination, backup, source, destination))
        return FALSE;
    GoldenHistoryEntry entry;
    golden_history_entry_resource(&entry, GOLDEN_HISTORY_REPLACE_MOVE_PNG,
                                  source, destination, backup);
    entry.staged = TRUE;
    golden_history_push_new(&g.history, &entry);
    update_state_after_resource_move(GOLDEN_HISTORY_MOVE_PNG,
                                     source, destination);
    select_resource_after_refresh(destination);
    return TRUE;
}

static void update_state_after_resource_move(GoldenHistoryKind kind,
                                             const wchar_t *source,
                                             const wchar_t *destination) {
    BOOL active_changed = FALSE;
    if (kind == GOLDEN_HISTORY_MOVE_PNG) {
        if (!_wcsicmp(g.image_path, source)) {
            if (!golden_path_copy(destination, g.image_path,
                                  _countof(g.image_path)) ||
                !parent_dir_for(destination, g.current_dir,
                                _countof(g.current_dir))) return;
            active_changed = TRUE;
        }
    } else {
        active_changed = replace_path_prefix(g.image_path,
            _countof(g.image_path), source, destination);
        replace_path_prefix(g.current_dir, _countof(g.current_dir),
                            source, destination);
    }
    if (active_changed) update_title_for_active_resource();
    else update_status();
}

static BOOL apply_resource_history(GoldenHistoryEntry *entry, BOOL undo) {
    if (entry->kind == GOLDEN_HISTORY_CREATE_DIRECTORY) {
        wchar_t parent[MAX_PATH * 4];
        if (!parent_dir_for(entry->destination, parent, _countof(parent)))
            return FALSE;
        if (undo) {
            if (!RemoveDirectoryW(entry->destination)) {
                show_error(GetLastError() == ERROR_DIR_NOT_EMPTY ?
                    L"The folder is no longer empty. Undo its contents first, then retry." :
                    L"Windows could not undo the folder creation.");
                return FALSE;
            }
            if (!_wcsicmp(g.current_dir, entry->destination)) {
                if (!golden_path_copy(parent, g.current_dir,
                                      _countof(g.current_dir))) return FALSE;
            }
            update_status();
            select_resource_after_refresh(parent);
            return TRUE;
        }
        if (!CreateDirectoryW(entry->destination, NULL)) {
            show_error(GetLastError() == ERROR_ALREADY_EXISTS ?
                L"A file or folder now occupies the folder path, so the creation cannot be redone." :
                L"Windows could not redo the folder creation.");
            return FALSE;
        }
        select_resource_after_refresh(entry->destination);
        return TRUE;
    }

    if (entry->kind == GOLDEN_HISTORY_CREATE_PNG) {
        const wchar_t *source = undo ? entry->source : entry->destination;
        const wchar_t *destination = undo ? entry->destination : entry->source;
        GoldenResourceRenameResult result = move_resource_pair(source, destination);
        if (result != GOLDEN_RENAME_OK) {
            show_error(resource_pair_error(result));
            return FALSE;
        }
        entry->staged = undo;
        if (undo && !_wcsicmp(g.image_path, entry->source)) {
            wchar_t parent[MAX_PATH * 4];
            if (!parent_dir_for(entry->source, parent, _countof(parent)))
                return FALSE;
            clear_active_resource(parent);
        } else if (!undo) {
            if (!load_png(entry->source)) {
                show_error(L"The captured PNG was restored but could not be decoded.");
                g.resource_visible = FALSE;
            } else {
                g.resource_visible = TRUE;
            }
            if (!golden_path_copy(entry->source, g.image_path,
                                  _countof(g.image_path)) ||
                !parent_dir_for(entry->source, g.current_dir,
                                _countof(g.current_dir))) return FALSE;
            load_annotations(entry->source);
            update_title_for_active_resource();
        }
        select_resource_after_refresh(undo ? g.current_dir : entry->source);
        update_tool_availability();
        InvalidateRect(g.editor, NULL, FALSE);
        return TRUE;
    }

    if (entry->kind == GOLDEN_HISTORY_DELETE_PNG) {
        const wchar_t *source = undo ? entry->destination : entry->source;
        const wchar_t *destination = undo ? entry->source : entry->destination;
        GoldenResourceRenameResult result = move_resource_pair(source, destination);
        if (result != GOLDEN_RENAME_OK) {
            show_error(resource_pair_error(result));
            return FALSE;
        }
        entry->staged = !undo;
        if (undo) {
            if (!g.image_path[0] && !load_resource(entry->source)) {
                show_error(L"The deleted PNG was restored but could not be opened.");
                select_resource_after_refresh(entry->source);
                return TRUE;
            }
            select_resource_after_refresh(entry->source);
        } else {
            wchar_t parent[MAX_PATH * 4];
            if (!parent_dir_for(entry->source, parent, _countof(parent)))
                parent[0] = 0;
            if (!_wcsicmp(g.image_path, entry->source))
                clear_active_resource(parent[0] ? parent : NULL);
            select_resource_after_refresh(parent[0] ? parent : g.root);
        }
        return TRUE;
    }

    if (entry->kind == GOLDEN_HISTORY_DELETE_DIRECTORY) {
        const wchar_t *source = undo ? entry->destination : entry->source;
        const wchar_t *destination = undo ? entry->source : entry->destination;
        BOOL deleting_active = !undo && g.image_path[0] &&
            golden_path_is_same_or_inside(g.image_path, entry->source);
        if (deleting_active) {
            if (!maybe_save() ||
                !golden_path_copy(g.image_path, entry->auxiliary,
                                  _countof(entry->auxiliary))) return FALSE;
        }
        GoldenDirectoryMoveResult result =
            golden_move_directory(source, destination);
        if (result != GOLDEN_DIRECTORY_MOVE_OK) {
            show_error(result == GOLDEN_DIRECTORY_MOVE_DESTINATION_EXISTS ?
                undo ?
                    L"The original folder path is occupied, so the deletion cannot be undone." :
                    L"The undo-storage path is occupied, so the folder deletion cannot be redone." :
                undo ? L"Windows could not undo the folder deletion." :
                       L"Windows could not redo the folder deletion.");
            return FALSE;
        }
        entry->staged = !undo;
        if (undo) {
            const wchar_t *selection = entry->source;
            if (entry->auxiliary[0] && !g.image_path[0]) {
                if (!load_resource(entry->auxiliary))
                    show_error(L"The deleted folder was restored, but its previously open PNG could not be reopened.");
                else
                    selection = entry->auxiliary;
            }
            select_resource_after_refresh(selection);
        } else {
            wchar_t parent[MAX_PATH * 4];
            if (!parent_dir_for(entry->source, parent, _countof(parent)))
                parent[0] = 0;
            if (deleting_active) {
                golden_history_remove_annotations(&g.history);
                clear_active_resource(parent[0] ? parent : NULL);
            } else if (golden_path_is_same_or_inside(g.current_dir,
                                                     entry->source)) {
                golden_path_copy(parent[0] ? parent : g.root, g.current_dir,
                                 _countof(g.current_dir));
                update_status();
            }
            select_resource_after_refresh(parent[0] ? parent : g.root);
        }
        return TRUE;
    }

    if (entry->kind == GOLDEN_HISTORY_REPLACE_MOVE_PNG) {
        BOOL moved = undo ? move_two_resource_pairs(
            entry->destination, entry->source,
            entry->auxiliary, entry->destination) :
            move_two_resource_pairs(
                entry->destination, entry->auxiliary,
                entry->source, entry->destination);
        if (!moved) return FALSE;
        entry->staged = !undo;
        const wchar_t *source = undo ? entry->destination : entry->source;
        const wchar_t *destination = undo ? entry->source : entry->destination;
        update_state_after_resource_move(GOLDEN_HISTORY_MOVE_PNG,
                                         source, destination);
        select_resource_after_refresh(destination);
        return TRUE;
    }

    const wchar_t *source = undo ? entry->destination : entry->source;
    const wchar_t *destination = undo ? entry->source : entry->destination;
    if (entry->kind == GOLDEN_HISTORY_MOVE_PNG) {
        GoldenResourceRenameResult result =
            move_resource_pair(source, destination);
        if (result != GOLDEN_RENAME_OK) {
            show_error(resource_pair_error(result));
            return FALSE;
        }
    } else if (entry->kind == GOLDEN_HISTORY_MOVE_DIRECTORY) {
        GoldenDirectoryMoveResult result =
            golden_move_directory(source, destination);
        if (result != GOLDEN_DIRECTORY_MOVE_OK) {
            show_error(result == GOLDEN_DIRECTORY_MOVE_DESTINATION_EXISTS ?
                L"The original path is occupied, so the folder move cannot be reversed." :
                L"Windows could not reverse the folder move.");
            return FALSE;
        }
    } else {
        return FALSE;
    }
    update_state_after_resource_move(entry->kind, source, destination);
    select_resource_after_refresh(destination);
    return TRUE;
}

static BOOL rename_resource_directory(const wchar_t *old_path,
                                      const wchar_t *edited_name) {
    if (!_wcsicmp(old_path, g.root)) {
        show_error(L"The open resource root cannot be renamed from inside Goldens.");
        return FALSE;
    }
    wchar_t name[256];
    wcsncpy(name, edited_name, _countof(name) - 1);
    name[_countof(name) - 1] = 0;
    trim_text(name);
    if (!valid_resource_name(name)) {
        show_error(L"Enter a valid Windows folder name.");
        return FALSE;
    }
    wchar_t parent[MAX_PATH * 4], new_path[MAX_PATH * 4];
    if (!parent_dir_for(old_path, parent, _countof(parent))) {
        show_error(L"The folder path is too long.");
        return FALSE;
    }
    if (!child_path(parent, name, new_path, _countof(new_path))) {
        show_error(L"The folder path is too long.");
        return FALSE;
    }
    if (!wcscmp(old_path, new_path)) return TRUE;
    GoldenDirectoryMoveResult result = golden_move_directory(old_path, new_path);
    if (result != GOLDEN_DIRECTORY_MOVE_OK) {
        show_error(result == GOLDEN_DIRECTORY_MOVE_DESTINATION_EXISTS ?
            L"A file or folder with that name already exists here." :
            L"Windows could not rename the folder.");
        return FALSE;
    }
    push_resource_undo(GOLDEN_HISTORY_MOVE_DIRECTORY, old_path, new_path);
    update_state_after_resource_move(GOLDEN_HISTORY_MOVE_DIRECTORY, old_path, new_path);
    select_resource_after_refresh(new_path);
    return TRUE;
}

static BOOL move_resource_to_directory(ResourceTreeNode *source,
                                       ResourceTreeNode *destination) {
    if (!source || !source->path || !destination || !destination->path ||
        destination->kind != RESOURCE_DIRECTORY ||
        source->kind == RESOURCE_ANNOTATION) return FALSE;
    if (source->kind == RESOURCE_DIRECTORY && !_wcsicmp(source->path, g.root)) {
        show_error(L"The open resource root cannot be moved from inside Goldens.");
        return FALSE;
    }
    if (source->kind == RESOURCE_DIRECTORY &&
        golden_path_is_same_or_inside(destination->path, source->path)) {
        show_error(L"A folder cannot be moved into itself or one of its subfolders.");
        return FALSE;
    }
    wchar_t old_parent[MAX_PATH * 4];
    if (!parent_dir_for(source->path, old_parent, _countof(old_parent))) {
        show_error(L"The resource path is too long.");
        return FALSE;
    }
    if (!_wcsicmp(old_parent, destination->path)) return TRUE;

    wchar_t new_path[MAX_PATH * 4];
    if (!child_path(destination->path, PathFindFileNameW(source->path),
                    new_path, _countof(new_path))) {
        show_error(L"The destination path is too long.");
        return FALSE;
    }
    if (source->kind == RESOURCE_PNG) {
        GoldenResourceRenameResult result =
            move_resource_pair(source->path, new_path);
        if (result == GOLDEN_RENAME_PNG_EXISTS)
            return replace_resource_after_confirmation(source->path, new_path);
        if (result != GOLDEN_RENAME_OK) {
            show_error(resource_pair_error(result));
            return FALSE;
        }
        push_resource_undo(GOLDEN_HISTORY_MOVE_PNG, source->path, new_path);
        update_state_after_resource_move(GOLDEN_HISTORY_MOVE_PNG, source->path, new_path);
    } else {
        GoldenDirectoryMoveResult result =
            golden_move_directory(source->path, new_path);
        if (result != GOLDEN_DIRECTORY_MOVE_OK) {
            const wchar_t *message =
                result == GOLDEN_DIRECTORY_MOVE_DESTINATION_EXISTS ?
                L"A file or folder with that name already exists in the destination folder." :
                result == GOLDEN_DIRECTORY_MOVE_DESTINATION_INSIDE_SOURCE ?
                L"A folder cannot be moved into itself or one of its subfolders." :
                L"Windows could not move the folder.";
            show_error(message);
            return FALSE;
        }
        push_resource_undo(GOLDEN_HISTORY_MOVE_DIRECTORY, source->path, new_path);
        update_state_after_resource_move(GOLDEN_HISTORY_MOVE_DIRECTORY,
                                         source->path, new_path);
    }
    select_resource_after_refresh(new_path);
    return TRUE;
}

static BOOL rename_resource_file(const wchar_t *old_path, const wchar_t *edited_name) {
    wchar_t name[256];
    wcsncpy(name, edited_name, _countof(name) - 1);
    name[_countof(name) - 1] = 0;
    normalize_capture_name(name);
    if (!valid_resource_name(name)) {
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

    GoldenResourceRenameResult result = move_resource_pair(old_path, new_path);
    if (result == GOLDEN_RENAME_PNG_EXISTS)
        return replace_resource_after_confirmation(old_path, new_path);
    if (result != GOLDEN_RENAME_OK) {
        show_error(resource_pair_error(result));
        return FALSE;
    }
    push_resource_undo(GOLDEN_HISTORY_MOVE_PNG, old_path, new_path);
    update_state_after_resource_move(GOLDEN_HISTORY_MOVE_PNG, old_path, new_path);
    select_resource_after_refresh(new_path);
    return TRUE;
}

static BOOL begin_tree_rename(HTREEITEM item) {
    ResourceTreeNode *node = tree_node_data(item);
    if (!node || (node->kind != RESOURCE_DIRECTORY &&
                  node->kind != RESOURCE_PNG &&
                  node->kind != RESOURCE_ANNOTATION) ||
        (node->kind == RESOURCE_DIRECTORY && !_wcsicmp(node->path, g.root)))
        return FALSE;
    if (node->kind == RESOURCE_ANNOTATION &&
        node->annotation_index >= 0 && node->annotation_index < g.annotation_count) {
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

static void cancel_resource_drag(void) {
    if (g.tree) TreeView_SelectDropTarget(g.tree, NULL);
    g.resource_dragging = FALSE;
    g.resource_drag_source = NULL;
    g.resource_drop_target = NULL;
}

static void update_resource_drag_target(POINT main_client) {
    POINT tree_point = main_client;
    MapWindowPoints(g.main, g.tree, &tree_point, 1);
    TVHITTESTINFO hit = {0};
    hit.pt = tree_point;
    TreeView_HitTest(g.tree, &hit);
    HTREEITEM directory_item = hit.hItem ? directory_item_for(hit.hItem) : NULL;
    ResourceTreeNode *source = tree_node_data(g.resource_drag_source);
    ResourceTreeNode *destination = tree_node_data(directory_item);
    if (!source || !destination || destination->kind != RESOURCE_DIRECTORY ||
        (source->kind == RESOURCE_DIRECTORY &&
         golden_path_is_same_or_inside(destination->path, source->path)))
        directory_item = NULL;
    g.resource_drop_target = directory_item;
    TreeView_SelectDropTarget(g.tree, directory_item);
    SetCursor(LoadCursorW(NULL, directory_item ? IDC_ARROW : IDC_NO));
}

static void finish_resource_drag(void) {
    ResourceTreeNode *source = tree_node_data(g.resource_drag_source);
    ResourceTreeNode *destination = tree_node_data(g.resource_drop_target);
    g.resource_dragging = FALSE;
    TreeView_SelectDropTarget(g.tree, NULL);
    g.resource_drag_source = NULL;
    g.resource_drop_target = NULL;
    if (GetCapture() == g.main) ReleaseCapture();
    if (source && destination) move_resource_to_directory(source, destination);
}

static BOOL make_history_temporary_path(const wchar_t *extension,
                                        wchar_t *path, size_t capacity) {
    static volatile LONG sequence;
    wchar_t directory[MAX_PATH * 4];
    if (!GetTempPathW(_countof(directory), directory)) return FALSE;
    for (int attempt = 0; attempt < 128; ++attempt) {
        LONG value = InterlockedIncrement(&sequence);
        int length = _snwprintf(path, capacity,
            L"%sgoldens-history-%08lx-%08lx%s", directory,
            (unsigned long)GetCurrentProcessId(), (unsigned long)value,
            extension ? extension : L"");
        if (length < 0 || (size_t)length >= capacity) return FALSE;
        HANDLE reservation = CreateFileW(path, GENERIC_WRITE, 0, NULL,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (reservation != INVALID_HANDLE_VALUE) {
            CloseHandle(reservation);
            return DeleteFileW(path);
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    }
    return FALSE;
}

static BOOL make_history_temporary_directory_path(wchar_t *path,
                                                  size_t capacity) {
    static volatile LONG sequence;
    wchar_t container[MAX_PATH * 4], container_name[64];
    int length = _snwprintf(container_name, _countof(container_name),
                            L".goldens-history-%08lx",
                            (unsigned long)GetCurrentProcessId());
    if (length < 0 || (size_t)length >= _countof(container_name) ||
        !golden_path_join(g.root, container_name,
                          container, _countof(container))) return FALSE;
    DWORD attributes = GetFileAttributesW(container);
    BOOL created = FALSE;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(container, NULL)) return FALSE;
        created = TRUE;
        attributes = GetFileAttributesW(container);
    }
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (created) RemoveDirectoryW(container);
        return FALSE;
    }
    DWORD hidden = (attributes | FILE_ATTRIBUTE_HIDDEN |
                    FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) &
                   ~(DWORD)FILE_ATTRIBUTE_NORMAL;
    if (!SetFileAttributesW(container, hidden)) {
        if (created) RemoveDirectoryW(container);
        return FALSE;
    }
    for (int attempt = 0; attempt < 128; ++attempt) {
        LONG value = InterlockedIncrement(&sequence);
        wchar_t name[64];
        length = _snwprintf(name, _countof(name), L"folder-%08lx",
                            (unsigned long)value);
        if (length < 0 || (size_t)length >= _countof(name) ||
            !golden_path_join(container, name, path, capacity)) return FALSE;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND ||
                error == ERROR_PATH_NOT_FOUND) return TRUE;
        }
    }
    return FALSE;
}

static BOOL resource_pair_path_available(const wchar_t *png_path) {
    wchar_t json_path[MAX_PATH * 4];
    return png_path &&
        GetFileAttributesW(png_path) == INVALID_FILE_ATTRIBUTES &&
        golden_resource_json_path(png_path, json_path, _countof(json_path)) &&
        GetFileAttributesW(json_path) == INVALID_FILE_ATTRIBUTES;
}

static BOOL make_unique_copy_path(const wchar_t *source,
                                  const wchar_t *directory,
                                  wchar_t *destination, size_t capacity) {
    wchar_t stem[256];
    const wchar_t *filename = PathFindFileNameW(source);
    wcsncpy(stem, filename, _countof(stem) - 1);
    stem[_countof(stem) - 1] = 0;
    if (ends_with_png(stem)) stem[wcslen(stem) - 4] = 0;
    for (int index = 0; index < 10000; ++index) {
        wchar_t candidate[256], suffix[32];
        int suffix_length = 0;
        suffix[0] = 0;
        if (index > 0)
            suffix_length = _snwprintf(suffix, _countof(suffix),
                                       L"-%d", index);
        if (suffix_length < 0 || (size_t)suffix_length >= _countof(suffix))
            return FALSE;
        size_t stem_length = wcslen(stem);
        size_t maximum_stem = 251u - (size_t)suffix_length;
        if (stem_length > maximum_stem) stem_length = maximum_stem;
        int length = _snwprintf(candidate, _countof(candidate), L"%.*s%s",
                                (int)stem_length, stem, suffix);
        if (length < 0 || (size_t)length >= _countof(candidate)) continue;
        if (!golden_path_join_extension(directory, candidate, L".png",
                                        destination, capacity)) return FALSE;
        if (resource_pair_path_available(destination)) return TRUE;
    }
    return FALSE;
}

static void record_created_png(const wchar_t *path,
                               const wchar_t *staged_path) {
    GoldenHistoryEntry entry;
    golden_history_entry_resource(&entry, GOLDEN_HISTORY_CREATE_PNG,
                                  path, staged_path, NULL);
    golden_history_push_new(&g.history, &entry);
    wchar_t parent[MAX_PATH * 4];
    if (parent_dir_for(path, parent, _countof(parent)))
        refresh_resources_expanding(parent);
    else
        refresh_resources();
    if (load_resource(path)) {
        HTREEITEM item = find_resource_item(TreeView_GetRoot(g.tree), path);
        if (item) {
            g.rebuilding_resources = TRUE;
            TreeView_EnsureVisible(g.tree, item);
            TreeView_SelectItem(g.tree, item);
            g.rebuilding_resources = FALSE;
        }
    }
}

static void delete_selected_resource(void) {
    ResourceTreeNode *node = selected_tree_node();
    if (!node || !node->path) return;
    if (node->kind == RESOURCE_DIRECTORY) {
        if (!_wcsicmp(node->path, g.root)) return;
        wchar_t source[MAX_PATH * 4], parent[MAX_PATH * 4];
        wchar_t staged[MAX_PATH * 4], active_image[MAX_PATH * 4] = L"";
        if (!golden_path_copy(node->path, source, _countof(source)) ||
            !parent_dir_for(source, parent, _countof(parent))) {
            show_error(L"The selected folder path is too long.");
            return;
        }
        wchar_t message[MAX_PATH * 4 + 256];
        _snwprintf(message, _countof(message),
            L"Delete the folder '%s' and everything inside it?\n\n"
            L"You can undo this with Ctrl+Z.", PathFindFileNameW(source));
        if (MessageBoxW(g.main, message, APP_NAME,
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
        BOOL active_inside = g.image_path[0] &&
            golden_path_is_same_or_inside(g.image_path, source);
        if (active_inside) {
            if (!maybe_save()) return;
            if (!golden_path_copy(g.image_path, active_image,
                                  _countof(active_image))) return;
        }
        if (!make_history_temporary_directory_path(staged,
                                                   _countof(staged))) {
            show_error(L"Goldens could not reserve recoverable undo storage for the folder deletion.");
            return;
        }
        GoldenDirectoryMoveResult result =
            golden_move_directory(source, staged);
        if (result != GOLDEN_DIRECTORY_MOVE_OK) {
            cleanup_staged_directory(staged, FALSE);
            show_error(result == GOLDEN_DIRECTORY_MOVE_DESTINATION_EXISTS ?
                L"Goldens could not reserve a unique undo location for the folder." :
                L"Windows could not move the folder into recoverable undo storage.");
            return;
        }
        if (active_inside) {
            golden_history_remove_annotations(&g.history);
            clear_active_resource(parent);
        } else if (golden_path_is_same_or_inside(g.current_dir, source)) {
            golden_path_copy(parent, g.current_dir, _countof(g.current_dir));
            update_status();
        }
        GoldenHistoryEntry entry;
        golden_history_entry_resource(&entry,
            GOLDEN_HISTORY_DELETE_DIRECTORY, source, staged, active_image);
        entry.staged = TRUE;
        golden_history_push_new(&g.history, &entry);
        select_resource_after_refresh(parent);
        return;
    }
    if (node->kind != RESOURCE_PNG) return;
    wchar_t source[MAX_PATH * 4], parent[MAX_PATH * 4], staged[MAX_PATH * 4];
    if (!golden_path_copy(node->path, source, _countof(source)) ||
        !parent_dir_for(source, parent, _countof(parent))) {
        show_error(L"The selected resource path is too long.");
        return;
    }
    if (!_wcsicmp(source, g.image_path) && !maybe_save()) return;
    if (!make_history_temporary_path(L".png", staged, _countof(staged))) {
        show_error(L"Goldens could not reserve recoverable undo storage for the deletion.");
        return;
    }
    GoldenResourceRenameResult result = move_resource_pair(source, staged);
    if (result != GOLDEN_RENAME_OK) {
        show_error(resource_pair_error(result));
        return;
    }
    if (!_wcsicmp(source, g.image_path)) {
        golden_history_remove_annotations(&g.history);
        clear_active_resource(parent);
    }
    GoldenHistoryEntry entry;
    golden_history_entry_resource(&entry, GOLDEN_HISTORY_DELETE_PNG,
                                  source, staged, NULL);
    entry.staged = TRUE;
    golden_history_push_new(&g.history, &entry);
    select_resource_after_refresh(parent);
}

static void copy_image_to_clipboard(void) {
    BYTE *pixels = active_pixels();
    if (!pixels || !active_width() || !active_height() || !active_stride()) return;
    const wchar_t *path = g.resource_visible && g.image_path[0] ?
                          g.image_path : NULL;
    if (!golden_clipboard_copy_image(g.main, pixels, active_width(),
                                     active_height(), active_stride(), path))
        show_error(L"Windows could not copy the image to the clipboard.");
    update_menu_availability();
}

static void paste_image_from_clipboard(void) {
    const wchar_t *directory = selected_directory_path();
    DWORD attributes = directory ? GetFileAttributesW(directory) :
                                   INVALID_FILE_ATTRIBUTES;
    if (!directory || attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        show_error(L"Select an available resource directory before pasting.");
        return;
    }
    if (!maybe_save()) return;
    GoldenImage image = {0};
    wchar_t source[MAX_PATH * 4], destination[MAX_PATH * 4];
    GoldenClipboardContent content = golden_clipboard_read_image(
        g.main, &image, source, _countof(source));
    if (content == GOLDEN_CLIPBOARD_NONE) {
        show_error(L"The clipboard does not contain a supported image.");
        return;
    }
    wchar_t staged[MAX_PATH * 4];
    if (!make_history_temporary_path(L".png", staged, _countof(staged))) {
        golden_image_free(&image);
        show_error(L"Goldens could not reserve recoverable undo storage for the pasted image.");
        return;
    }
    BOOL created = FALSE;
    if (content == GOLDEN_CLIPBOARD_PNG_PATH) {
        GoldenImage validated = {0};
        if (!golden_png_load(g.wic, source, &validated)) {
            show_error(L"The PNG on the clipboard could not be decoded.");
        } else if (!make_unique_copy_path(source, directory, destination,
                                          _countof(destination))) {
            show_error(L"Goldens could not find an available name for the pasted PNG.");
        } else if (!golden_copy_resource_pair(source, destination)) {
            show_error(L"Windows could not copy the PNG and its JSON sidecar.");
        } else {
            created = TRUE;
        }
        golden_image_free(&validated);
    } else {
        wchar_t name[256] = L"pasted-image";
        if (prompt_text(g.main, L"Paste image",
                        L"Resource name (without .png):", name,
                        _countof(name))) {
            normalize_capture_name(name);
            if (!valid_resource_name(name)) {
                show_error(L"Enter a valid Windows resource name.");
            } else if (!golden_path_join_extension(directory, name, L".png",
                                                   destination,
                                                   _countof(destination))) {
                show_error(L"The pasted resource path is too long.");
            } else if (!resource_pair_path_available(destination)) {
                show_error(L"A PNG or JSON sidecar with that name already exists.");
            } else if (!save_png_pixels(destination, image.pixels, image.width,
                                        image.height, image.stride)) {
                show_error(L"Goldens could not save the pasted image.");
            } else {
                created = TRUE;
            }
        }
    }
    golden_image_free(&image);
    if (created) record_created_png(destination, staged);
}

static const wchar_t *capture_bundle_error(
    GoldenCaptureBundleStatus status) {
    switch (status) {
    case GOLDEN_CAPTURE_BUNDLE_DESTINATION_EXISTS:
        return L"A file or folder with that bundle name already exists.";
    case GOLDEN_CAPTURE_BUNDLE_CREATE_FAILED:
        return L"Goldens could not create temporary storage for the capture bundle.";
    case GOLDEN_CAPTURE_BUNDLE_NO_VISIBLE_WINDOWS:
        return L"Goldens could not find a visible foreground window to capture.";
    case GOLDEN_CAPTURE_BUNDLE_SCREEN_CAPTURE_FAILED:
        return L"Windows could not capture the foreground application's visible scene.";
    case GOLDEN_CAPTURE_BUNDLE_SAVE_FAILED:
        return L"Goldens could not save every PNG and annotation sidecar in the bundle.";
    case GOLDEN_CAPTURE_BUNDLE_MANIFEST_FAILED:
        return L"Goldens captured the images but could not write the bundle manifest.";
    case GOLDEN_CAPTURE_BUNDLE_FINALIZE_FAILED:
        return L"Goldens captured the bundle but Windows could not move it into the selected folder.";
    default:
        return L"Goldens could not create the capture bundle.";
    }
}

static void select_loaded_resource(const wchar_t *path) {
    HTREEITEM item = find_resource_item(TreeView_GetRoot(g.tree), path);
    if (!item) return;
    g.rebuilding_resources = TRUE;
    TreeView_EnsureVisible(g.tree, item);
    TreeView_SelectItem(g.tree, item);
    g.rebuilding_resources = FALSE;
}

static void set_capture_hotkey_enabled(BOOL enabled, BOOL remember) {
    if (enabled && !g.capture_hotkey_registered) {
        if (RegisterHotKey(g.main, CAPTURE_HOTKEY_ID,
                           CAPTURE_HOTKEY_MODIFIERS,
                           CAPTURE_HOTKEY_KEY)) {
            g.capture_hotkey_registered = TRUE;
        } else {
            enabled = FALSE;
            show_error(L"F8 is already in use, so background capture was turned off.");
        }
    } else if (!enabled && g.capture_hotkey_registered) {
        UnregisterHotKey(g.main, CAPTURE_HOTKEY_ID);
        g.capture_hotkey_registered = FALSE;
    }
    g.capture_hotkey_enabled = enabled;
    if (g.capture_menu)
        CheckMenuItem(g.capture_menu, ID_CAPTURE_LISTEN,
                      MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
    update_status();
    if (remember) remember_capture_hotkey_setting();
}

static BOOL make_pending_capture_path(const wchar_t *directory,
                                      wchar_t *path, size_t capacity) {
    static volatile LONG sequence;
    for (int attempt = 0; attempt < 128; ++attempt) {
        wchar_t leaf[96];
        LONG value = InterlockedIncrement(&sequence);
        int length = _snwprintf(
            leaf, _countof(leaf), L".goldens-pending-%08lx-%08lx",
            (unsigned long)GetCurrentProcessId(), (unsigned long)value);
        if (length < 0 || (size_t)length >= _countof(leaf) ||
            !golden_path_join(directory, leaf, path, capacity)) return FALSE;
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

static void discard_pending_capture(const wchar_t *path) {
    if (golden_delete_directory_tree(path)) return;
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    show_error(L"The capture was cancelled, but Goldens could not remove its temporary bundle.");
}

static BOOL publish_pending_capture(const wchar_t *directory,
                                    const wchar_t *pending,
                                    wchar_t *destination,
                                    size_t capacity) {
    wchar_t name[256] = L"Capture Bundle";
    for (;;) {
        if (!prompt_text(g.main, L"Save capture bundle", L"Bundle folder name:",
                         name, _countof(name))) {
            discard_pending_capture(pending);
            return FALSE;
        }
        trim_text(name);
        if (!valid_resource_name(name)) {
            show_error(L"Enter a valid Windows folder name.");
            continue;
        }
        if (!golden_path_join(directory, name, destination, capacity)) {
            show_error(L"The capture bundle path is too long.");
            continue;
        }
        wchar_t scene_path[MAX_PATH * 4];
        if (!golden_path_join_extension(destination, L"scene", L".png",
                                        scene_path, _countof(scene_path))) {
            show_error(L"The capture bundle path is too long.");
            continue;
        }
        if (GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES) {
            show_error(L"A file or folder with that bundle name already exists.");
            continue;
        }
        if (MoveFileExW(pending, destination, MOVEFILE_WRITE_THROUGH)) {
            SetFileAttributesW(destination, FILE_ATTRIBUTE_NORMAL);
            return TRUE;
        }
        DWORD error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
            show_error(L"A file or folder with that bundle name already exists.");
            continue;
        }
        SetFileAttributesW(pending, FILE_ATTRIBUTE_NORMAL);
        show_error(L"Windows could not name the captured bundle. The temporary bundle remains in the capture folder.");
        return FALSE;
    }
}

static void capture_foreground_bundle(void) {
    if (!g.capture_hotkey_registered || g.capture_in_progress) return;
    HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == g.main) return;

    const wchar_t *selected_directory = selected_directory_path();
    wchar_t directory[MAX_PATH * 4];
    DWORD attributes = selected_directory ?
        GetFileAttributesW(selected_directory) : INVALID_FILE_ATTRIBUTES;
    if (!selected_directory || attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !golden_path_copy(selected_directory, directory,
                          _countof(directory))) {
        ShowWindow(g.main, SW_RESTORE);
        SetForegroundWindow(g.main);
        show_error(L"Open or select an available resource folder before pressing F8.");
        return;
    }

    g.capture_in_progress = TRUE;
    wchar_t pending[MAX_PATH * 4] = L"";
    GoldenCaptureBundleResult result = {0};
    GoldenCaptureBundleStatus status = GOLDEN_CAPTURE_BUNDLE_CREATE_FAILED;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!make_pending_capture_path(directory, pending,
                                       _countof(pending))) break;
        status = golden_capture_bundle_create(
            g.wic, foreground, pending, &result);
        if (status != GOLDEN_CAPTURE_BUNDLE_DESTINATION_EXISTS) break;
    }

    ShowWindow(g.main, SW_RESTORE);
    SetForegroundWindow(g.main);
    if (status != GOLDEN_CAPTURE_BUNDLE_OK) {
        g.capture_in_progress = FALSE;
        show_error(capture_bundle_error(status));
        return;
    }
    SetFileAttributesW(pending, FILE_ATTRIBUTE_HIDDEN);

    wchar_t destination[MAX_PATH * 4];
    if (!publish_pending_capture(directory, pending, destination,
                                 _countof(destination))) {
        g.capture_in_progress = FALSE;
        refresh_resources();
        return;
    }

    wchar_t scene_path[MAX_PATH * 4];
    BOOL has_scene = golden_path_join_extension(
        destination, L"scene", L".png", scene_path, _countof(scene_path));
    golden_path_copy(destination, g.current_dir, _countof(g.current_dir));
    refresh_resources_expanding(destination);
    if (has_scene && load_resource(scene_path))
        select_loaded_resource(scene_path);
    g.capture_in_progress = FALSE;
    update_status();
    update_menu_availability();
    MessageBeep(MB_OK);
}

static void toggle_resource_pane(void) {
    g.left_collapsed = !g.left_collapsed;
    layout_children(g.main);
    InvalidateRect(g.left_splitter, NULL, TRUE);
}

static LRESULT CALLBACK SplitterProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client;
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
        RECT indicator = client;
        if (g.left_collapsed) {
            int thickness = max(3, client.right / 4);
            indicator.left = indicator.right - thickness;
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
        g.splitter_dragging = TRUE;
        g.splitter_drag_start = (POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &g.splitter_drag_start);
        g.splitter_width_start = 0;
        if (!g.left_collapsed) {
            RECT pane;
            GetWindowRect(g.tree, &pane);
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
            int width = g.splitter_width_start + delta;
            BOOL next_collapsed = golden_pane_should_collapse(
                width, GOLDEN_RESOURCE_PANE_MIN, g.left_collapsed);
            if (next_collapsed != g.left_collapsed) {
                g.left_collapsed = next_collapsed;
                InvalidateRect(hwnd, NULL, TRUE);
            }
            if (!next_collapsed)
                g.left_column_width = max(
                    GOLDEN_RESOURCE_PANE_MIN, min(520, width));
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
        toggle_resource_pane();
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
    GoldenToolIcon icon = (GoldenToolIcon)index;
    if (index == TOOL_CLICK && g.tool == TOOL_CLICK && click_clear_available())
        icon = GOLDEN_TOOL_ICON_CLEAR_CLICK;
    golden_draw_tool_icon(item->hDC, icon, &icon_bounds,
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
        g.left_column_width, g.left_collapsed);
    ShowWindow(g.tree, g.left_collapsed ? SW_HIDE : SW_SHOWNA);
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
    PLACE_CONTROL(g.status, layout.status);
#undef PLACE_CONTROL
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void set_tool_with_focus(ToolMode tool, BOOL focus_editor) {
    BOOL resource_available = g.resource_visible &&
                              selected_active_resource_node() != NULL;
    if (!resource_available) {
        update_tool_availability();
        return;
    }
    if (tool == TOOL_CLICK && (g.selected < 0 ||
                               g.selected >= g.annotation_count)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    g.tool = tool;
    update_tool_availability();
    if (focus_editor) SetFocus(g.editor);
}

static void set_tool(ToolMode tool) {
    if (tool == TOOL_CLICK && g.tool == TOOL_CLICK && click_clear_available()) {
        clear_click();
        SetFocus(g.editor);
        return;
    }
    set_tool_with_focus(tool, TRUE);
}

static void update_tool_availability(void) {
    BOOL active_resource_selected = selected_active_resource_node() != NULL;
    BOOL resource_selected = g.resource_visible && active_resource_selected;
    if (!g.tool_buttons[0]) {
        update_menu_availability();
        return;
    }
    BOOL click_available = resource_selected && g.selected >= 0 &&
                           g.selected < g.annotation_count;
    if (!click_available && g.tool == TOOL_CLICK) g.tool = TOOL_SELECT;
    for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i) {
        BOOL available = resource_selected &&
                         (i != TOOL_CLICK || click_available);
        EnableWindow(g.tool_buttons[i], available);
        InvalidateRect(g.tool_buttons[i], NULL, FALSE);
    }
    const wchar_t *click_tooltip =
        g.tool == TOOL_CLICK && click_clear_available() ?
        L"Clear click point (click again)" : L"Place or move click point";
    if (wcscmp(g.click_tooltip_text, click_tooltip)) {
        wcsncpy(g.click_tooltip_text, click_tooltip,
                _countof(g.click_tooltip_text) - 1);
        g.click_tooltip_text[_countof(g.click_tooltip_text) - 1] = 0;
        if (g.tool_tooltip && g.tool_button_tooltips[TOOL_CLICK].uId)
            SendMessageW(g.tool_tooltip, TTM_UPDATETIPTEXTW, 0,
                         (LPARAM)&g.tool_button_tooltips[TOOL_CLICK]);
    }
    update_menu_availability();
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
    case ID_NEW_FOLDER: if (g.root[0]) create_folder(); break;
    case ID_SAVE: if (g.dirty && g.image_path[0]) save_annotations(); break;
    case ID_EXIT: SendMessageW(g.main, WM_CLOSE, 0, 0); break;
    case ID_UNDO: undo_action(); break;
    case ID_REDO: redo_action(); break;
    case ID_COPY: {
        HWND edit = TreeView_GetEditControl(g.tree);
        if (edit && GetFocus() == edit) SendMessageW(edit, WM_COPY, 0, 0);
        else copy_image_to_clipboard();
        break;
    }
    case ID_PASTE: {
        HWND edit = TreeView_GetEditControl(g.tree);
        if (edit && GetFocus() == edit) SendMessageW(edit, WM_PASTE, 0, 0);
        else paste_image_from_clipboard();
        break;
    }
    case ID_RENAME: {
        if (!tree_rename_available()) break;
        HWND focus = GetFocus();
        HTREEITEM selected = TreeView_GetSelection(g.tree);
        if ((focus == g.tree || IsChild(g.tree, focus)) && begin_tree_rename(selected)) break;
        rename_selected();
        break;
    }
    case ID_DELETE: {
        HWND edit = TreeView_GetEditControl(g.tree);
        if (edit && GetFocus() == edit)
            SendMessageW(edit, WM_KEYDOWN, VK_DELETE, 0);
        else if (resource_delete_available()) delete_selected_resource();
        else if (annotation_action_available()) delete_selected();
        break;
    }
    case ID_CLEAR_CLICK: if (click_clear_available()) clear_click(); break;
    case ID_FIT:
        if (active_pixels()) {
            g.zoom = 0.0; g.pan_x = g.pan_y = 0;
            InvalidateRect(g.editor, NULL, FALSE);
        }
        break;
    case ID_ZOOM_OUT: if (active_pixels()) zoom_by(0.8, NULL); break;
    case ID_ZOOM_IN: if (active_pixels()) zoom_by(1.25, NULL); break;
    case ID_ACTUAL:
        if (active_pixels()) {
            g.zoom = 1.0; g.pan_x = g.pan_y = 0;
            InvalidateRect(g.editor, NULL, FALSE);
        }
        break;
    case ID_CAPTURE_LISTEN:
        set_capture_hotkey_enabled(!g.capture_hotkey_enabled, TRUE);
        break;
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
    AppendMenuW(file, MF_STRING, ID_NEW_FOLDER, L"New Folder…\tCtrl+N");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_SAVE, L"Save Annotations\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, ID_EXIT, L"Exit\tCtrl+Q");
    AppendMenuW(edit, MF_STRING, ID_UNDO, L"Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, ID_REDO, L"Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, ID_COPY, L"Copy Image\tCtrl+C");
    AppendMenuW(edit, MF_STRING, ID_PASTE, L"Paste Image\tCtrl+V");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, ID_RENAME, L"Rename Selection\tF2");
    AppendMenuW(edit, MF_STRING, ID_DELETE, L"Delete\tDel");
    AppendMenuW(edit, MF_STRING, ID_CLEAR_CLICK, L"Clear Click Point");
    AppendMenuW(view, MF_STRING, ID_FIT, L"Fit Image\t0");
    AppendMenuW(view, MF_STRING, ID_ACTUAL, L"Actual Size\t1");
    AppendMenuW(view, MF_STRING, ID_ZOOM_IN, L"Zoom In\tCtrl++");
    AppendMenuW(view, MF_STRING, ID_ZOOM_OUT, L"Zoom Out\tCtrl+-");
    AppendMenuW(capture, MF_STRING | (g.capture_hotkey_enabled ?
                MF_CHECKED : MF_UNCHECKED), ID_CAPTURE_LISTEN,
                L"Listen for F8");
    g.file_menu = file;
    g.edit_menu = edit;
    g.view_menu = view;
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
        g.selected = node->annotation_index;
        set_tool_with_focus(TOOL_SELECT, FALSE);
        InvalidateRect(g.editor, NULL, FALSE);
    }
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.main = hwnd;
        SetMenu(hwnd, create_main_menu());
        g.context_label = CreateWindowW(L"STATIC", L"  No resource selected", WS_CHILD | WS_VISIBLE |
            SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS | SS_NOPREFIX,
            0, 0, 0, 0, hwnd, NULL, g.instance, NULL);

        const wchar_t *tool_labels[] = {L"Select", L"Rectangle", L"Click"};
        const wchar_t *tool_tips[] = {L"Select, move, resize, or pan",
                                      L"Rectangle", g.click_tooltip_text};
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
        g.status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 0, 0, hwnd, NULL, g.instance, NULL);
        g.left_splitter = CreateWindowW(L"GoldensSplitter", NULL,
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_SPLITTER_LEFT, g.instance, NULL);
        set_tool(TOOL_SELECT);
        update_context_label();
        if (g.root[0]) {
            restart_resource_watcher();
            refresh_resources();
        }
        update_status();
        update_tool_availability();
        set_capture_hotkey_enabled(g.capture_hotkey_enabled, TRUE);
        AddClipboardFormatListener(hwnd);
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
        if (wp == RESOURCE_TREE_TIMER) {
            if (g.resource_dragging) {
                if (!SetTimer(hwnd, RESOURCE_TREE_TIMER,
                              RESOURCE_TREE_COALESCE_MS, NULL))
                    PostMessageW(hwnd, WM_TIMER, RESOURCE_TREE_TIMER, 0);
                return 0;
            }
            KillTimer(hwnd, RESOURCE_TREE_TIMER);
            BOOL watcher_started = g.resource_watcher.implementation != NULL;
            if (g.resource_watcher_needs_restart) {
                golden_resource_watcher_stop(&g.resource_watcher, 2000);
                g.resource_watcher_needs_restart = FALSE;
                watcher_started = g.root[0] && golden_resource_watcher_start(
                    &g.resource_watcher, g.root, g.main,
                    WM_RESOURCE_TREE_CHANGED);
                if (!watcher_started)
                    g.resource_watcher_needs_restart = TRUE;
            }
            LONG generation_before = golden_resource_watcher_generation(
                &g.resource_watcher);
            refresh_resources();
            update_status();
            golden_resource_watcher_acknowledge(&g.resource_watcher);
            LONG generation_after = golden_resource_watcher_generation(
                &g.resource_watcher);
            g.resource_refresh_pending = FALSE;
            if (generation_after != generation_before)
                schedule_resource_refresh(RESOURCE_TREE_COALESCE_MS);
            if (!watcher_started && g.root[0]) {
                g.resource_watcher_needs_restart = TRUE;
                schedule_resource_refresh(RESOURCE_TREE_RETRY_MS);
            }
        }
        return 0;
    case WM_INITMENUPOPUP:
        update_menu_availability();
        return 0;
    case WM_COMMAND:
        handle_command(LOWORD(wp));
        return 0;
    case WM_HOTKEY:
        if (wp == CAPTURE_HOTKEY_ID) {
            capture_foreground_bundle();
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (g.resource_dragging) {
            update_resource_drag_target((POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g.resource_dragging) {
            finish_resource_drag();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g.resource_dragging) cancel_resource_drag();
        break;
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
            SetTextColor((HDC)wp,
                         g.resource_visible && g.image_path[0] ? RGB(0, 105, 145) :
                         GetSysColor(COLOR_BTNTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_CLIPBOARDUPDATE:
        update_menu_availability();
        return 0;
    case WM_RESOURCE_TREE_CHANGED:
        if (wp) g.resource_watcher_needs_restart = TRUE;
        schedule_resource_refresh(
            wp ? RESOURCE_TREE_RETRY_MS : RESOURCE_TREE_COALESCE_MS);
        return 0;
    case WM_RESOURCES_CHANGED: {
        refresh_resources();
        if (g.pending_resource_selection[0]) {
            HTREEITEM item = find_resource_item(TreeView_GetRoot(g.tree),
                                                g.pending_resource_selection);
            ResourceTreeNode *selected_node = tree_node_data(item);
            g.rebuilding_resources = TRUE;
            if (item) {
                TreeView_EnsureVisible(g.tree, item);
                TreeView_SelectItem(g.tree, item);
            }
            g.rebuilding_resources = FALSE;
            if (selected_node && selected_node->kind == RESOURCE_DIRECTORY)
                activate_resource_node(selected_node);
            g.pending_resource_selection[0] = 0;
        }
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
            return !(node && ((node->kind == RESOURCE_DIRECTORY &&
                               _wcsicmp(node->path, g.root)) ||
                              node->kind == RESOURCE_PNG ||
                              node->kind == RESOURCE_ANNOTATION));
        }
        if (header->idFrom == ID_TREE && header->code == TVN_ENDLABELEDITW) {
            NMTVDISPINFOW *edit = (NMTVDISPINFOW *)lp;
            ResourceTreeNode *node = tree_node_data(edit->item.hItem);
            if (!node || !edit->item.pszText) return FALSE;
            if (node->kind == RESOURCE_DIRECTORY && node->path)
                return rename_resource_directory(node->path, edit->item.pszText);
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
                    if (!push_undo()) return FALSE;
                    wcscpy(g.annotations[node->annotation_index].name, name);
                    update_dirty_state();
                    update_tool_availability();
                    InvalidateRect(g.editor, NULL, FALSE);
                    PostMessageW(g.main, WM_ANNOTATION_RENAMED, 0, 0);
                }
                return TRUE;
            }
            return FALSE;
        }
        if (header->idFrom == ID_TREE && header->code == TVN_BEGINDRAGW) {
            NMTREEVIEWW *drag = (NMTREEVIEWW *)lp;
            ResourceTreeNode *node = tree_node_data(drag->itemNew.hItem);
            if (node && (node->kind == RESOURCE_PNG ||
                         (node->kind == RESOURCE_DIRECTORY &&
                          _wcsicmp(node->path, g.root)))) {
                g.resource_dragging = TRUE;
                g.resource_drag_source = drag->itemNew.hItem;
                g.resource_drop_target = NULL;
                SetCapture(g.main);
                POINT point = drag->ptDrag;
                MapWindowPoints(g.tree, g.main, &point, 1);
                update_resource_drag_target(point);
            }
            return 0;
        }
        if (header->idFrom == ID_TREE && header->code == TVN_SELCHANGEDW &&
            !g.rebuilding_resources) {
            NMTREEVIEWW *change = (NMTREEVIEWW *)lp;
            ResourceTreeNode *node = (ResourceTreeNode *)change->itemNew.lParam;
            if (node) activate_resource_node(node);
            else {
                BOOL was_showing_resource = g.resource_visible;
                g.resource_visible = FALSE;
                g.selected = -1;
                update_tool_availability();
                if (was_showing_resource) {
                    update_context_label();
                    InvalidateRect(g.editor, NULL, FALSE);
                }
            }
            update_tool_availability();
        }
        if (header->idFrom == ID_TREE && header->code == NM_CLICK) {
            ResourceTreeNode *node = clicked_resource_node();
            BOOL current_png = node && node->kind == RESOURCE_PNG && node->path &&
                               !_wcsicmp(node->path, g.image_path);
            BOOL current_annotation = node && node->kind == RESOURCE_ANNOTATION &&
                node->annotation_index >= 0 && node->annotation_index < g.annotation_count;
            if (current_png || current_annotation) activate_resource_node(node);
        }
        if (header->code == NM_CLICK && header->idFrom == ID_TREE)
            clear_tree_selection_on_blank_click(header->hwndFrom);
        if (header->idFrom == ID_TREE && header->code == NM_DBLCLK) {
            DWORD position = GetMessagePos();
            TVHITTESTINFO hit = {0};
            hit.pt = (POINT){GET_X_LPARAM(position), GET_Y_LPARAM(position)};
            ScreenToClient(g.tree, &hit.pt);
            TreeView_HitTest(g.tree, &hit);
            ResourceTreeNode *node = tree_node_data(hit.hItem);
            if (hit.hItem && (hit.flags & TVHT_ONITEMLABEL) && node &&
                (node->kind == RESOURCE_PNG || node->kind == RESOURCE_ANNOTATION))
                PostMessageW(g.main, WM_BEGIN_TREE_RENAME, 0, (LPARAM)hit.hItem);
        }
        return 0;
    }
    case WM_CLOSE:
        if (maybe_save()) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g.capture_hotkey_registered)
            UnregisterHotKey(hwnd, CAPTURE_HOTKEY_ID);
        RemoveClipboardFormatListener(hwnd);
        cancel_resource_drag();
        KillTimer(hwnd, RESOURCE_TREE_TIMER);
        golden_resource_watcher_stop(&g.resource_watcher, 2000);
        if (g.editor_tooltip) {
            DestroyWindow(g.editor_tooltip);
            g.editor_tooltip = NULL;
        }
        if (g.tool_tooltip) {
            DestroyWindow(g.tool_tooltip);
            g.tool_tooltip = NULL;
        }
        free_tree_item(g.tree, TreeView_GetRoot(g.tree));
        clear_image();
        golden_history_destroy(&g.history);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    initialize_app_state(instance);
    golden_history_init(&g.history, discard_history_entry, NULL);
    load_capture_hotkey_setting();
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
    main_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_GOLDENS));
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
        {FVIRTKEY | FCONTROL, 'O', ID_OPEN},
        {FVIRTKEY | FCONTROL, 'N', ID_NEW_FOLDER},
        {FVIRTKEY | FCONTROL, 'S', ID_SAVE},
        {FVIRTKEY | FCONTROL, 'Q', ID_EXIT},
        {FVIRTKEY | FCONTROL, 'Z', ID_UNDO}, {FVIRTKEY | FCONTROL, 'Y', ID_REDO},
        {FVIRTKEY | FCONTROL, 'C', ID_COPY}, {FVIRTKEY | FCONTROL, 'V', ID_PASTE},
        {FVIRTKEY, VK_F2, ID_RENAME},
        {FVIRTKEY, VK_DELETE, ID_DELETE},
        {FVIRTKEY | FCONTROL, VK_OEM_MINUS, ID_ZOOM_OUT},
        {FVIRTKEY | FCONTROL, VK_OEM_PLUS, ID_ZOOM_IN},
        {FVIRTKEY | FCONTROL, VK_SUBTRACT, ID_ZOOM_OUT},
        {FVIRTKEY | FCONTROL, VK_ADD, ID_ZOOM_IN}
    };
    HACCEL accelerators = CreateAcceleratorTableW(shortcuts, _countof(shortcuts));
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        HWND tree_edit = TreeView_GetEditControl(g.tree);
        BOOL editing_shortcut = tree_edit && msg.hwnd == tree_edit &&
            msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_DELETE ||
             (GetKeyState(VK_CONTROL) < 0 &&
              (msg.wParam == 'C' || msg.wParam == 'V' ||
               msg.wParam == 'Z' || msg.wParam == 'Y')));
        if (editing_shortcut ||
            !TranslateAcceleratorW(g.main, accelerators, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(accelerators);
    IWICImagingFactory_Release(g.wic);
    CoUninitialize();
    return (int)msg.wParam;
}
