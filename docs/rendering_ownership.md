# Explorer Rendering Ownership

This document maps out the visual components of `programs/explorer/` in the Wine codebase to identify their rendering owners and clarify what components can be modernized entirely within the `programs/explorer` module.

---

## 1. Component Ownership & Portability Map

| UI Element / Component | File & Function Location | Renderer Component | Owner / Module | Modifiable in `programs/explorer`? |
| :--- | :--- | :--- | :--- | :--- |
| **Virtual Desktop Background** | `desktop.c:desktop_wnd_proc` (`WM_PAINT`, `WM_ERASEBKGND`) | GDI `PaintDesktop` / Desktop brush | `user32` / `programs/explorer` | **Yes** |
| **Desktop Icons / Launchers** | `desktop.c:draw_launchers` | GDI `DrawIconEx` & `DrawTextW` | `programs/explorer` | **Yes** |
| **Taskbar Main Window (`Shell_TrayWnd`)** | `systray.c:shell_traywnd_proc` | Standard GDI fill / Custom layout | `programs/explorer` | **Yes** |
| **Start Button** | `systray.c:paint_taskbar_button` | GDI `DrawFrameControl` & `DrawCaptionTempW` | `programs/explorer` | **Yes** |
| **Taskbar Window Buttons** | `systray.c:paint_taskbar_button` | GDI `DrawFrameControl` & `DrawCaptionTempW` | `programs/explorer` | **Yes** |
| **System Tray Standalone Window** | `systray.c:initialize_systray` | Standard Window Frame / Win32 | `user32` / `programs/explorer` | **Yes** |
| **Tray Icons (Standard & Layered)** | `systray.c:tray_icon_wndproc` (`WM_PAINT`, `paint_layered_icon`) | GDI `DrawIconEx` / `UpdateLayeredWindow` | `programs/explorer` | **Yes** |
| **Tray Balloon Notifications** | `systray.c:balloon_create_timer` | Tooltips Control | `comctl32` | **No** (Controlled via COM/common controls) |
| **Tray Icon Tooltips** | `systray.c:create_tooltip` | Tooltips Control | `comctl32` | **No** (Controlled via COM/common controls) |
| **Start Menu Shell Shortcuts** | `startmenu.c:menu_wndproc` (`WM_DRAWITEM`) | GDI `ImageList_Draw` / Menu Engine | `user32` / `programs/explorer` | **Yes** (Drawn natively inside `menu_wndproc`) |
| **Explorer Browser Frame** | `explorer.c:make_explorer_window` | `IExplorerBrowser` COM Server | `shell32` | **No** (Core rendering inside `shell32.dll`) |
| **Explorer Rebar Control** | `explorer.c:make_explorer_window` | Rebar Engine | `comctl32` | **No** (Controlled via common controls) |
| **Explorer Navigation Toolbar** | `explorer.c:make_explorer_window` | Toolbar Engine | `comctl32` | **No** (Controlled via common controls) |
| **Explorer Address/Path Combobox** | `explorer.c:make_explorer_window` | ComboBoxEx Control | `comctl32` | **No** (Controlled via common controls) |

---

## 2. Rendering Strategy Analysis

### 2.1 elements rendered 100% natively inside `programs/explorer`
These elements are registered, managed, and drawn with code directly written in `programs/explorer`:
1. **Desktop Launcher Icons:** Drawn inside `desktop.c:draw_launchers` using standard Windows GDI text and icon paint calls (`DrawIconEx` and `DrawTextW`).
2. **Taskbar Windows & Buttons:** The `Shell_TrayWnd` window frame, layout, sizing logic, and the standard buttons (the Start Button and open window tiles) are drawn customly inside `systray.c:paint_taskbar_button` using GDI drawing functions.
3. **Tray Icon Windows:** In standalone mode, the custom `__wine_tray_icon` window handles its own painting natively within `tray_icon_wndproc` via `DrawIconEx`. Layered icons use GDI section bits with alpha calculations written in `paint_layered_icon` before calling `UpdateLayeredWindow`.
4. **Custom Owner-Drawn Menu Elements:** Individual item icons within the Start Menu are rendered inside `startmenu.c` via `ImageList_Draw`.

---

### 2.2 Elements Delegating Drawing to External DLLs
The following elements utilize standard Win32 structures and delegate actual drawing to system libraries:
1. **Explorer File Browsing View Pane:** Instantiated through `IExplorerBrowser` (via COM `CLSID_ExplorerBrowser`). All lists, detail tables, folders, and selection drawings are executed inside `shell32.dll` or `comctl32.dll`.
2. **Rebar, Toolbar, and ComboBoxEx Containers:** Instantiated in `explorer.c` using window classes `REBARCLASSNAMEW`, `TOOLBARCLASSNAMEW`, and `WC_COMBOBOXEXW`. They delegate window procedures, theme management, and rendering to `comctl32.dll`.
3. **Tray Tooltips and Balloons:** The actual balloon bubbles and tooltip popups are created with standard class `TOOLTIPS_CLASSW` and have their appearance drawn by `comctl32.dll`.
4. **Start Menu Shell Wrapper:** Created using standard menus via `CreatePopupMenu`. Standard menu backgrounds, highlights, and layouts are drawn by `user32.dll` (with `programs/explorer` intercepting item drawing for shortcuts).

---

### 2.3 Visual Elements Modifiable/Modernizable Entirely Within `programs/explorer`
To modernize Wine's virtual desktop interface entirely inside `programs/explorer` without making invasive changes to base libraries like `user32` or `comctl32`, you can refactor or replace rendering for:
1. **Virtual Desktop Wallpapers & Layouts:** Rewrite `desktop.c:desktop_wnd_proc` to replace standard GDI-based `PaintDesktop` and draw custom backgrounds, grid structures, widgets, or Direct2D/GDI+ modern graphics.
2. **Desktop Shortcut Icons:** Refactor `desktop.c:draw_launchers` to draw modern UI icons, rounded bounding boxes on hover, anti-aliased labels, and high-DPI scaling.
3. **The Taskbar (`Shell_TrayWnd`):** Redesign the main taskbar paint loop. Instead of classic button bevels drawn with `DrawFrameControl`, implement a flat, translucent, or modern styled bar with modern spacing, custom layouts, hover effects, and modern icons.
4. **The Start Menu:** Replace the classic `CreatePopupMenu` shell start menu with a custom window class inside `startmenu.c` to draw a modern Start panel (supporting searchable items, pins, and custom CSS-like rendering) without needing menu hacks in `user32`.
