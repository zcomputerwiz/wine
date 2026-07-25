# Explorer Window Hierarchy

This document details every window class registered and window instance created directly within `programs/explorer/`. It maps out the parent-child relationships, creation details, procedures, and rendering delegation.

---

## 1. Window Classes and Instances Reference

### 1.1 `ExplorerWClass` (Main Explorer Window)
* **Window Class Name:** `L"ExplorerWClass"`
* **Parent Window:** `NULL` (WS_OVERLAPPEDWINDOW)
* **Creation Function:** `CreateWindowW` inside `explorer.c:make_explorer_window` (line 462)
* **Window Procedure:** `explorer_wnd_proc` (`explorer.c:719`)
* **Direct Painting:** **No** (It delegates sizes and notifies but does not have a `WM_PAINT` handler to draw pixels; background is handled via class background brush `(HBRUSH)COLOR_BACKGROUND`).
* **Delegated Rendering:** **Yes** (Delegates view panel to `IExplorerBrowser` / `shell32`, navigation buttons to `ToolbarWindow32` / `comctl32`, and address bar to `ComboBoxEx32` / `comctl32`).

---

### 1.2 `ReBarWindow32` (Rebar Control)
* **Window Class Name:** `REBARCLASSNAMEW` (`L"ReBarWindow32"`)
* **Parent Window:** `ExplorerWClass` (Main Window instance)
* **Creation Function:** `CreateWindowExW` inside `explorer.c:make_explorer_window` (line 481)
* **Window Procedure:** Common Control Procedure (`comctl32.dll`)
* **Direct Painting:** **No**
* **Delegated Rendering:** **Yes** (Common Controls library `comctl32.dll`)

---

### 1.3 `ToolbarWindow32` (Toolbar Control)
* **Window Class Name:** `TOOLBARCLASSNAMEW` (`L"ToolbarWindow32"`)
* **Parent Window:** `ReBarWindow32` (Rebar instance)
* **Creation Function:** `CreateWindowExW` inside `explorer.c:make_explorer_window` (line 485)
* **Window Procedure:** Common Control Procedure (`comctl32.dll`)
* **Direct Painting:** **No**
* **Delegated Rendering:** **Yes** (Common Controls library `comctl32.dll`)

---

### 1.4 `ComboBoxEx32` (Path Combo Box)
* **Window Class Name:** `WC_COMBOBOXEXW` (`L"ComboBoxEx32"`)
* **Parent Window:** `ReBarWindow32` (Rebar instance)
* **Creation Function:** `CreateWindowW` inside `explorer.c:make_explorer_window` (line 519)
* **Window Procedure:** Common Control Procedure (`comctl32.dll`)
* **Direct Painting:** **No**
* **Delegated Rendering:** **Yes** (Common Controls library `comctl32.dll`)

---

### 1.5 Virtual/Root Desktop Window
* **Window Class Name:** `DESKTOP_CLASS_ATOM` (`((LPCWSTR)MAKEINTATOM(32769))`)
* **Parent Window:** `HWND_DESKTOP` / `NULL`
* **Creation Function:** `CreateWindowExW` inside `desktop.c:manage_desktop` (line 1270)
* **Window Procedure:** Subclassed procedure `desktop_wnd_proc` (`desktop.c:832`) wraps the original class procedure `desktop_orig_wndproc`.
* **Direct Painting:** **Yes** (Handles `WM_PAINT` / `WM_ERASEBKGND` to paint wallpaper/desktop via `PaintDesktop` and manually draws launcher shortcuts via `draw_launchers` when in virtual desktop mode (`using_root` is false)).
* **Delegated Rendering:** **No** (Directly rendered inside `programs/explorer` using standard GDI Calls, e.g., `DrawIconEx`, `DrawTextW` on virtual desktop).

---

### 1.6 Message Receiver Window (Desktop Thread)
* **Window Class Name:** `L"Message"`
* **Parent Window:** `HWND_MESSAGE`
* **Creation Function:** `CreateWindowExW` inside `desktop.c:manage_desktop` (line 1276)
* **Window Procedure:** Default standard Message window procedure (`user32.dll`)
* **Direct Painting:** **No**
* **Delegated Rendering:** **Yes** (`user32.dll`)

---

### 1.7 `__wine_clipboard_manager` (Clipboard Window)
* **Window Class Name:** `L"__wine_clipboard_manager"`
* **Parent Window:** `HWND_MESSAGE`
* **Creation Function:** `CreateWindowW` inside `desktop.c:clipboard_thread` (line 698)
* **Window Procedure:** `clipboard_wndproc` (`desktop.c:655`) which relays key messages via `NtUserMessageCall` to `NtUserClipboardWindowProc`.
* **Direct Painting:** **No**
* **Delegated Rendering:** **No** (No visible UI / message-only window).

---

### 1.8 `__wine_display_settings_restorer` (Display Settings Restorer Window)
* **Window Class Name:** `L"__wine_display_settings_restorer"`
* **Parent Window:** `HWND_MESSAGE`
* **Creation Function:** `CreateWindowW` inside `desktop.c:display_settings_restorer_thread` (line 755)
* **Window Procedure:** `display_settings_restorer_wndproc` (`desktop.c:715`)
* **Direct Painting:** **No**
* **Delegated Rendering:** **No** (No visible UI / message-only window).

---

### 1.9 `WineAppBar` (AppBar Message Window)
* **Window Class Name:** `L"WineAppBar"`
* **Parent Window:** `HWND_MESSAGE`
* **Creation Function:** `CreateWindowW` inside `appbar.c:initialize_appbar` (line 305)
* **Window Procedure:** `appbar_wndproc` (`appbar.c:237`)
* **Direct Painting:** **No**
* **Delegated Rendering:** **No** (No visible UI / message-only window).

---

### 1.10 `Shell_TrayWnd` (Taskbar/System Tray Window)
* **Window Class Name:** `L"Shell_TrayWnd"`
* **Parent Window:** `NULL` (WS_POPUP)
* **Creation Function:** `CreateWindowExW` inside `systray.c:initialize_systray` (line 1255 or 1262 depending on taskbar mode)
* **Window Procedure:** `shell_traywnd_proc` (`systray.c:1130`)
* **Direct Painting:** **Yes** (Handles `WM_DRAWITEM` to delegate owner-drawn taskbar buttons and start button rendering).
* **Delegated Rendering:** **No** (Natively structures layout and receives notification messages).

---

### 1.11 `__wine_tray_icon` (System Tray Icon Adapter Window)
* **Window Class Name:** `L"__wine_tray_icon"`
* **Parent Window:** `Shell_TrayWnd` (WS_CHILD when nested in host tray) or `NULL` (when standalone popup)
* **Creation Function:** `CreateWindowExW` inside `systray.c:add_icon` (line 769)
* **Window Procedure:** `tray_icon_wndproc` (`systray.c:488`)
* **Direct Painting:** **Yes** (Manages `WM_PAINT` to render the icon image via `DrawIconEx`, or `paint_layered_icon` via `UpdateLayeredWindow` when using the layered window extension).
* **Delegated Rendering:** **No** (Entirely drawn within the window procedure or layered update sequence).

---

### 1.12 `Button` (Taskbar/Start Button)
* **Window Class Name:** `WC_BUTTONW` (`L"Button"`)
* **Parent Window:** `Shell_TrayWnd` (Taskbar window)
* **Creation Function:** `CreateWindowW` inside `systray.c:add_taskbar_button` (line 965)
* **Window Procedure:** Subclassed via standard `Button` window procedure with parent drawing notifications.
* **Direct Painting:** **Yes** (Owner-drawn; `Shell_TrayWnd` window procedure handles `WM_DRAWITEM` to paint the button background and window text via `paint_taskbar_button` using GDI).
* **Delegated Rendering:** **No** (Natively drawn inside `programs/explorer` with owner-draw styling).

---

### 1.13 Tooltip Controls (SysTray Tooltips & Balloons)
* **Window Class Name:** `TOOLTIPS_CLASSW` (`L"tooltips_class32"`)
* **Parent Window:** `__wine_tray_icon` (Tray icon instance)
* **Creation Function:** `CreateWindowExW` inside `systray.c:create_tooltip` (line 197) and `systray.c:balloon_create_timer` (line 240)
* **Window Procedure:** Common Control Procedure (`comctl32.dll`)
* **Direct Painting:** **No**
* **Delegated Rendering:** **Yes** (Common Controls library `comctl32.dll`)

---

### 1.14 Pop-up Start Menu
* **Window Class Name:** Standard Menu popup class (`#32768`)
* **Parent Window:** `Shell_TrayWnd` (Owner)
* **Creation Function:** `CreatePopupMenu` inside `startmenu.c:do_startmenu` (line 424)
* **Window Procedure:** Standard menu procedure handled internally by `user32.dll`, but custom items are drawn using the sub-classed `menu_wndproc` wrapper (`startmenu.c:338`) dispatched from the parent tray window.
* **Direct Painting:** **Yes** (Handles `WM_DRAWITEM` and `WM_MEASUREITEM` in `menu_wndproc` to render small icons next to shortcuts using GDI `ImageList_Draw`).
* **Delegated Rendering:** **Yes** (Dispatched through the standard menus in `user32.dll`).

---

## 2. Parent-Child Window Hierarchies

Explorer execution is split into two primary roles based on launch parameters: **Desktop Mode** (managing virtual desktop/systray/appbars) and **Explorer Mode** (managing file browsing windows).

### 2.1 Virtual Desktop Launch Sequence (Desktop Mode)

When launching a virtual desktop, the process registers internal classes and constructs the taskbar, tray area, appbars, and start menu:

```
HWND_DESKTOP (Root)
 ├── DESKTOP_CLASS_ATOM (Desktop Window; Proc: desktop_wnd_proc)
 │    └── __wine_tray_icon (SysTray Icon Window; Parent-swapped when docked)
 ├── Shell_TrayWnd (Taskbar / Tray Window; Proc: shell_traywnd_proc)
 │    ├── Button (Start Button Instance; BS_OWNERDRAW)
 │    ├── Button (Taskbar Window Button; BS_OWNERDRAW)
 │    └── __wine_tray_icon (Docked SysTray Icon Window; Proc: tray_icon_wndproc)
 │         ├── tooltips_class32 (Icon Tooltip Instance)
 │         └── tooltips_class32 (Balloon Notification Tooltip)
 └── HWND_MESSAGE
      ├── Message (Receives parent notifications)
      ├── __wine_clipboard_manager (Clipboard Sync)
      ├── __wine_display_settings_restorer (Multi-display)
      └── WineAppBar (AppBar registration broker)
```

### 2.2 Explorer Launch Sequence (File Browser Mode)

When browsing files, explorer registers the `ExplorerWClass` and hooks up shell-provided standard components:

```
HWND_DESKTOP (Root)
 └── ExplorerWClass (Main Window; Proc: explorer_wnd_proc)
      ├── ReBarWindow32 (Rebar Container)
      │    ├── ToolbarWindow32 (Standard navigation: Back/Forward/Up)
      │    └── ComboBoxEx32 (Dropdown path selector)
      └── [IExplorerBrowser Frame] (Comes from shell32.dll COM instance)
           └── ExplorerBrowser child windows (Lists, directory trees, etc.)
```
