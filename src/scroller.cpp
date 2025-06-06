#include <hyprland/src/desktop/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/devices/Keyboard.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#include "scroller.h"
#include "common.h"
#include "functions.h"
#include "row.h"
#include "column.h"
#include "overview.h"

#include <string>
#include <unordered_map>
#include <vector>


extern HANDLE PHANDLE;
extern std::unique_ptr<ScrollerLayout> g_ScrollerLayout;
extern Overview *overviews;

std::function<SDispatchResult(std::string)> orig_moveFocusTo;
std::function<SDispatchResult(std::string)> orig_moveActiveTo;

class Marks {
public:
    Marks() {}
    ~Marks() { reset(); }
    void reset() {
        marks.clear();
        post_mark_event(nullptr);
    }
    // Add a mark with name for window, overwriting any existing one with that name
    void add(PHLWINDOW window, const std::string &name) {
        const auto mark = marks.find(name);
        if (mark != marks.end()) {
            mark->second = window;
            post_mark_event(window);
            return;
        }
        marks[name] = window;
        post_mark_event(window);
    }
    void del(const std::string &name) {
        const auto mark = marks.find(name);
        if (mark != marks.end()) {
            if (g_pCompositor->m_lastWindow == mark->second)
                post_mark_event(nullptr);
            marks.erase(mark);
        }
    }
    // Remove window from list of marks (used when a window gets deleted)
    void remove(PHLWINDOW window) {
        for(auto it = marks.begin(); it != marks.end();) {
            if (it->second.lock() == window)
                it = marks.erase(it);
            else
                it++;
        }
    }
    // If the mark exists, returns that window, otherwise it returns null
    PHLWINDOW visit(const std::string &name) {
        const auto mark = marks.find(name);
        if (mark != marks.end()) {
            return mark->second.lock();
        }
        return nullptr;
    }

    void post_mark_event(PHLWINDOW window) {
        bool marked = false;
        for(auto it = marks.begin(); it != marks.end(); it++) {
            if (it->second.lock() == window) {
                g_pEventManager->postEvent(SHyprIPCEvent{"scroller", std::format("mark, 1, {}", it->first)});
                return;
            }
        }
        g_pEventManager->postEvent(SHyprIPCEvent{"scroller", "mark, 0, "});
    }

private:
    std::unordered_map<std::string, PHLWINDOWREF> marks;
};

static Marks marks;

class Trail {
protected:
    Trail(int number) : number(number), active(nullptr) {}
    ~Trail() {}

    void toggle(const PHLWINDOW window) {
        auto win = marks.first();
        while (win != nullptr) {
            auto next = win->next();
            if (win->data() == window) {
                active = active != marks.last() ? active->next() : active->prev();
                marks.erase(win);
                return;
            }
            win = next;
        }
        if (active == nullptr) {
            marks.push_back(window);
            active = marks.first();
        } else {
            marks.insert_after(active, window);
            active = active->next();
        }
    }
    void remove_window(PHLWINDOW window) {
        auto win = marks.first();
        while (win != nullptr) {
            auto next = win->next();
            if (win->data() == window) {
                active = active != marks.last() ? active->next() : active->prev();
                marks.erase(win);
                return;
            }
            win = next;
        }
    }
    void next() {
        if (active == nullptr)
            return;
        active = active == marks.last() ? marks.first() : active->next();
    }
    void prev() {
        if (active == nullptr)
            return;
        active = active == marks.first() ? marks.last() : active->prev();
    }
    void clear() {
        marks.clear();
        active = nullptr;
    }
    bool is_marked(PHLWINDOW window) const {
        for (auto win = marks.first(); win != nullptr; win = win->next()) {
            if (win->data() == window)
                return true;
        }
        return false;
    }
    void toselection() const {
        for (auto win = marks.first(); win != nullptr; win = win->next()) {
            g_ScrollerLayout->selection_set(win->data());
        }
        // Re-render windows to show decorations
        for (auto monitor : g_pCompositor->m_monitors) {
            g_pHyprRenderer->damageMonitor(monitor);
        }
    }

private:
    friend class Trails;

    int number;
    ListNode<const PHLWINDOWREF> *active;
    List<const PHLWINDOWREF> marks;
};

class Trails {
public:
    Trails() : counter(0), active(nullptr) {
        //trail_new();
    }
    ~Trails() {
        for (auto trail = trails.first(); trail != nullptr; trail = trail->next()) {
            delete trail->data();
        }
        active = nullptr;
        post_trailmark_event(nullptr);
        post_trail_event();
    }
    void remove_window(PHLWINDOW window) {
        for (auto trail = trails.first(); trail != nullptr; trail = trail->next()) {
            trail->data()->remove_window(window);
        }
        post_trail_event();
    }

    size_t get_active_size() const {
        return active ? active->data()->marks.size() : 0;
    }
    int get_active_number() const {
        return active ? active->data()->number : -1;
    }
    bool get_active_marked(PHLWINDOW window) const {
        return active ? active->data()->is_marked(window) : false;
    }
    PHLWINDOW get_active() const {
        if (active == nullptr) {
            return nullptr;
        } else {
            auto mark = active->data();
            if (mark->active != nullptr) {
                return mark->active->data().lock();
            } else {
                return nullptr;
            }
        }
    }
    void trail_new() {
        trails.push_back(new Trail(counter++));
        active = trails.last();
        post_trail_event();
    }
    void trail_next() {
        active = active == trails.last() ? trails.first() : active->next();
        post_trail_event();
    }
    void trail_prev() {
        active = active == trails.first() ? trails.last() : active->prev();
        post_trail_event();
    }
    void trail_delete() {
        if (active == nullptr)
            return;
        auto act = active == trails.first() ? active->next() : active->prev();
        trails.erase(active);
        delete active->data();
        active = act;
        post_trail_event();
    }
    void trail_clear() {
        if (active == nullptr)
            return;
        active->data()->clear();
        post_trail_event();
    }

    void trail_toselection() {
        if (active == nullptr)
            return;
        active->data()->toselection();
    }

    void trailmark_toggle(PHLWINDOW window) {
        if (active == nullptr) {
            trail_new();
        }
        active->data()->toggle(window);
        post_trailmark_event(window);
        post_trail_event();
    }
    void trailmark_next() {
        if (active == nullptr)
            return;
        active->data()->next();
    }
    void trailmark_prev() {
        if (active == nullptr)
            return;
        active->data()->prev();
    }

    void post_trail_event() {
        g_pEventManager->postEvent(SHyprIPCEvent{"scroller", std::format("trail, {}, {}", get_active_number(), get_active_size())});
    }
    void post_trailmark_event(PHLWINDOW window) {
        bool marked = false;
        if (active != nullptr && active->data()->is_marked(window))
            marked = true;
        g_pEventManager->postEvent(SHyprIPCEvent{"scroller", std::format("trailmark, {}", marked ? 1 : 0)});
    }

private:
    int counter;
    ListNode<Trail *> *active;
    List<Trail *> trails;
};

static Trails *trails;

// ScrollerLayout
Row *ScrollerLayout::getRowForWorkspace(WORKSPACEID workspace) {
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        if (row->data()->get_workspace() == workspace)
            return row->data();
    }
    return nullptr;
}

Row *ScrollerLayout::getRowForWindow(PHLWINDOW window) {
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        if (row->data()->has_window(window))
            return row->data();
    }
    return nullptr;
}

/*
    Called when a window is created (mapped)
    The layout HAS TO set the goal pos and size (anim mgr will use it)
    If !animationinprogress, then the anim mgr will not apply an anim.
*/
void ScrollerLayout::onWindowCreatedTiling(PHLWINDOW window, eDirection)
{
    WORKSPACEID wid = window->workspaceID();
    auto s = getRowForWorkspace(wid);
    if (s == nullptr) {
        s = new Row(wid);
        rows.push_back(s);
    }

    // Undo possible modifications from general options.
    window->unsetWindowData(PRIORITY_LAYOUT);
    window->updateWindowData();

    s->add_active_window(window);

    // Check window rules
    for (auto &r: window->m_matchedRules) {
        if (r->m_rule.starts_with("plugin:scroller:group")) {
            const auto name = r->m_rule.substr(r->m_rule.find_first_of(' ') + 1);
            s->move_active_window_to_group(name);
        } else if (r->m_rule.starts_with("plugin:scroller:alignwindow")) {
            const auto dir = r->m_rule.substr(r->m_rule.find_first_of(' ') + 1);
            if (dir == "l" || dir == "left") {
                s->align_column(Direction::Left);
            } else if (dir == "r" || dir == "right") {
                s->align_column(Direction::Right);
            } else if (dir == "u" || dir == "up") {
                s->align_column(Direction::Up);
            } else if (dir == "d" || dir == "dn" || dir == "down") {
                s->align_column(Direction::Down);
            } else if (dir == "c" || dir == "centre" || dir == "center") {
                s->align_column(Direction::Center);
            } else if (dir == "m" || dir == "middle") {
                s->align_column(Direction::Middle);
            }
        } else if (r->m_rule.starts_with("plugin:scroller:marksadd")) {
            const auto mark_name = r->m_rule.substr(r->m_rule.find_first_of(' ') + 1);
            marks.add(window, mark_name);
        }
    }
}

/*
    Called when a window is removed (unmapped) (m_isMapped still true), and
    then again when the window is destroyed.
    Some XWayland windows only call it once, at destroy, but those
    windows are not in the layout and are not floating either. For example Qt
    tooltips with a XWayland backend. What are they?
    Remove the window from the layout and re-focus in one call, so we can
    ignore windows that don't belong to the layout. However, if the window is
    removed because it became floating, we don't want to change focus to a
    tiled window, just remove it from the layout and let it keep focus.
*/
void ScrollerLayout::onWindowRemovedTiling(PHLWINDOW window)
{
    auto s = getRowForWindow(window);
    if (s == nullptr)
        return;

    marks.remove(window);
    trails->remove_window(window);

    if (!s->remove_window(window)) {
        // It was the last one, remove the row
        for (auto row = rows.first(); row != nullptr; row = row->next()) {
            if (row->data() == s) {
                rows.erase(row);
                delete row->data();
                break;
            }
        }
    }
    if (window->m_isFloating)
        return;

    // Don't modify focus if window is being dragged
    if (window == g_pInputManager->m_currentlyDraggedWindow)
        return;

    WORKSPACEID workspace_id = g_pCompositor->m_lastMonitor->activeSpecialWorkspaceID();
    if (!workspace_id) {
        workspace_id = g_pCompositor->m_lastMonitor->activeWorkspaceID();
    }
    s = getRowForWorkspace(workspace_id);
    if (s != nullptr)
        force_focus_to_window(s->get_active_window());
}

/*
    Called when a floating window is removed (unmapped)
*/
void ScrollerLayout::onWindowRemovedFloating(PHLWINDOW window)
{
    static auto* const *avoid_focus = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:avoid_focus_on_float_close")->getDataStaticPtr();
    static auto* const *avoid_focus_x11 = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:avoid_focus_on_xwayland_float_close")->getDataStaticPtr();
    if (**avoid_focus) {
        return;
    }
    if (window && window->m_isX11 && **avoid_focus_x11) {
        // Avoid automatic focus switch when an XWayland floating window is removed,
        // to prevent input method losing focus
        return;
    }
    WORKSPACEID workspace_id = g_pCompositor->m_lastMonitor->activeSpecialWorkspaceID();
    if (!workspace_id) {
        workspace_id = g_pCompositor->m_lastMonitor->activeWorkspaceID();
    }
    auto s = getRowForWorkspace(workspace_id);
    if (s != nullptr)
        g_pCompositor->focusWindow(s->get_active_window());
}

/*
    Internal: called when window focus changes
*/
void ScrollerLayout::onWindowFocusChange(PHLWINDOW window)
{
    if (window == nullptr) { // no window has focus
        return;
    }

    auto s = getRowForWindow(window);
    if (s == nullptr) {
        return;
    }
    s->focus_window(window);
}

/*
    Return tiled status
*/
bool ScrollerLayout::isWindowTiled(PHLWINDOW window)
{
    return getRowForWindow(window) != nullptr;
}

/*
    Called when the monitor requires a layout recalculation
    this usually means reserved area changes
*/
void ScrollerLayout::recalculateMonitor(const MONITORID &monitor_id)
{
    const auto PMONITOR = g_pCompositor->getMonitorFromID(monitor_id);
    if (!PMONITOR)
        return;

    g_pHyprRenderer->damageMonitor(PMONITOR);

    WORKSPACEID specialID = PMONITOR->activeSpecialWorkspaceID();
    if (specialID) {
        auto sw = getRowForWorkspace(specialID);
        if (sw == nullptr) {
            return;
        }
        const Box oldmax = sw->get_max();
        const bool force = sw->update_sizes(PMONITOR);
        auto PWORKSPACESPECIAL = PMONITOR->m_activeSpecialWorkspace;
        if (PWORKSPACESPECIAL->m_hasFullscreenWindow) {
            sw->set_fullscreen_mode_windows(PWORKSPACESPECIAL->m_fullscreenMode);
        } else {
            sw->update_windows(oldmax, force);
        }
    }

    auto PWORKSPACE = PMONITOR->m_activeWorkspace;
    if (!PWORKSPACE)
        return;

    auto s = getRowForWorkspace(PWORKSPACE->m_id);
    if (s == nullptr)
        return;

    const Box oldmax = s->get_max();
    const bool force = s->update_sizes(PMONITOR);
    if (PWORKSPACE->m_hasFullscreenWindow) {
        s->set_fullscreen_mode_windows(PWORKSPACE->m_fullscreenMode);
    } else {
        s->update_windows(oldmax, force);
    }
}

/*
    Called when the compositor requests a window
    to be recalculated, e.g. when pseudo is toggled.
*/
void ScrollerLayout::recalculateWindow(PHLWINDOW window)
{
    // It can get called after windows are already being destroyed (decorations update)
    if (!enabled)
        return;

    auto s = getRowForWindow(window);
    if (s == nullptr)
        return;

    s->recalculate_row_geometry();
}

/*
    Called when a user requests a resize of the current window by a vec
    Vector2D holds pixel values
    Optional pWindow for a specific window
*/
void ScrollerLayout::resizeActiveWindow(const Vector2D &delta,
                                        eRectCorner /* corner */, PHLWINDOW window)
{
    const auto PWINDOW = window ? window : g_pCompositor->m_lastWindow.lock();
    auto s = getRowForWindow(PWINDOW);
    if (s == nullptr) {
        // Window is not tiled
        *PWINDOW->m_realSize = Vector2D(std::max((PWINDOW->m_realSize->goal() + delta).x, 20.0), std::max((PWINDOW->m_realSize->goal() + delta).y, 20.0));
        PWINDOW->sendWindowSize();
        PWINDOW->updateWindowDecos();
        return;
    }

    s->resize_active_window(delta);
}

/*
   Called when a window / the user requests to toggle the fullscreen state of a
   window. The layout sets all the fullscreen flags. It can either accept or
   ignore.
*/
void ScrollerLayout::fullscreenRequestForWindow(PHLWINDOW window,
                                                const eFullscreenMode CURRENT_EFFECTIVE_MODE,
                                                const eFullscreenMode EFFECTIVE_MODE)
{
    auto s = getRowForWindow(window);

    if (s == nullptr) {
        // save position and size if floating
        if (window->m_isFloating && CURRENT_EFFECTIVE_MODE == FSMODE_NONE) {
            window->m_lastFloatingSize     = window->m_realSize->goal();
            window->m_lastFloatingPosition = window->m_realPosition->goal();
            window->m_position             = window->m_realPosition->goal();
            window->m_size                 = window->m_realSize->goal();
        }
        if (EFFECTIVE_MODE == FSMODE_NONE) {
            // window is not tiled
            if (window->m_isFloating) {
                // get back its' dimensions from position and size
                *window->m_realPosition = window->m_lastFloatingPosition;
                *window->m_realSize     = window->m_lastFloatingSize;

                window->unsetWindowData(PRIORITY_LAYOUT);
                window->updateWindowData();
                window->sendWindowSize();
            }
        } else {
            // apply new pos and size being monitors' box
            const auto PMONITOR   = window->m_monitor.lock();
            if (EFFECTIVE_MODE == FSMODE_FULLSCREEN) {
                *window->m_realPosition = PMONITOR->m_position;
                *window->m_realSize     = PMONITOR->m_size;
            } else {
                Box box = { PMONITOR->m_position + PMONITOR->m_reservedTopLeft,
                            PMONITOR->m_size - PMONITOR->m_reservedTopLeft - PMONITOR->m_reservedBottomRight};
                *window->m_realPosition = Vector2D(box.x, box.y);
                *window->m_realSize = Vector2D(box.w, box.h);
                window->sendWindowSize();
            }
        }
    } else {
        if (EFFECTIVE_MODE == CURRENT_EFFECTIVE_MODE)
            return;
        s->set_fullscreen_mode(window, CURRENT_EFFECTIVE_MODE, EFFECTIVE_MODE);
    }
    g_pCompositor->changeWindowZOrder(window, true);
}

/*
    Called when a dispatcher requests a custom message
    The layout is free to ignore.
    std::any is the reply. Can be empty.
*/
std::any ScrollerLayout::layoutMessage(SLayoutMessageHeader /* header */, std::string /* content */)
{
    return "";
}

/*
    Required to be handled, but may return just SWindowRenderLayoutHints()
    Called when the renderer requests any special draw flags for
    a specific window, e.g. border color for groups.
*/
SWindowRenderLayoutHints ScrollerLayout::requestRenderHints(PHLWINDOW)
{
    return {};
}

/*
    Called when the user requests two windows to be swapped places.
    The layout is free to ignore.
*/
void ScrollerLayout::switchWindows(PHLWINDOW, PHLWINDOW)
{
}

/*
    Called when the user requests a window move in a direction.
    The layout is free to ignore.
*/
void ScrollerLayout::moveWindowTo(PHLWINDOW window, const std::string &direction, bool /* silent */)
{
    auto s = getRowForWindow(window);
    if (s == nullptr) {
        return;
    } else if (!(s->is_active(window))) {
        // cannot move non active window?
        return;
    }

    switch (direction.at(0)) {
        case 'l': s->move_active_column(Direction::Left); break;
        case 'r': s->move_active_column(Direction::Right); break;
        case 'u': s->move_active_column(Direction::Up); break;
        case 'd': s->move_active_column(Direction::Down); break;
        default: break;
    }

    // "silent" requires to keep focus in the neighborhood of the moved window
    // before it moved. I ignore it for now.
}

/*
    Called when the user requests to change the splitratio by or to X
    on a window
*/
void ScrollerLayout::alterSplitRatio(PHLWINDOW, float, bool)
{
}

/*
    Called when something wants the current layout's name
*/
std::string ScrollerLayout::getLayoutName()
{
    return "scroller";
}

/*
    Called for getting the next candidate for a focus
*/
PHLWINDOW ScrollerLayout::getNextWindowCandidate(PHLWINDOW/* old_window */)
{
    // This is called when a windows in unmapped. This means the window
    // has also been removed from the layout. In that case, returning the
    // new active window is the correct thing.
    // We would like to be able to retain the full screen mode for old_window's
    // workspace if it was a different one than the current one (background
    // window unmapped), but old_window has had its fsmode removed in
    // Hyprland's /src/events/Windows.cpp
    // void Events::listener_unmapWindow(void* owner, void* data);
    // so it is impossible to know the old state short of storing it ourselves
    // in Row, because WORKSPACE has also lost it. Storing it in Row is hard
    // to keep synchronized. So for now, unmapping a window from a workspace
    // different than the active one, loses full screen state.
    WORKSPACEID workspace_id = g_pCompositor->m_lastMonitor->activeSpecialWorkspaceID();
    if (!workspace_id) {
        workspace_id = g_pCompositor->m_lastMonitor->activeWorkspaceID();
    }
    auto s = getRowForWorkspace(workspace_id);
    if (s == nullptr)
        return nullptr;
    else
        return s->get_active_window();
}

/*
    Called for replacing any data a layout has for a new window
*/
void ScrollerLayout::replaceWindowDataWith(PHLWINDOW /* from */, PHLWINDOW /* to */)
{
}

static SP<HOOK_CALLBACK_FN> workspaceHookCallback;
static SP<HOOK_CALLBACK_FN> focusedMonHookCallback;
static SP<HOOK_CALLBACK_FN> activeWindowHookCallback;
static SP<HOOK_CALLBACK_FN> swipeBeginHookCallback;
static SP<HOOK_CALLBACK_FN> swipeUpdateHookCallback;
static SP<HOOK_CALLBACK_FN> swipeEndHookCallback;
static SP<HOOK_CALLBACK_FN> mouseMoveHookCallback;

void ScrollerLayout::onEnable() {
    // Hijack Hyprland's default dispatchers
    orig_moveFocusTo = g_pKeybindManager->m_dispatchers["movefocus"];
    orig_moveActiveTo = g_pKeybindManager->m_dispatchers["movewindow"];
    g_pKeybindManager->m_dispatchers["movefocus"] = this_moveFocusTo;
    g_pKeybindManager->m_dispatchers["movewindow"] = this_moveActiveTo;

    // Register dynamic callbacks for events
    workspaceHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "workspace", [&](void* /* self */, SCallbackInfo& /* info */, std::any param) {
        auto WORKSPACE = std::any_cast<PHLWORKSPACE>(param);
        post_event(WORKSPACE->m_id, "mode");
        post_event(WORKSPACE->m_id, "overview");
    });
    focusedMonHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "focusedMon", [&](void* /* self */, SCallbackInfo& /* info */, std::any param) {
        auto monitor = std::any_cast<PHLMONITOR>(param);
        post_event(monitor->activeWorkspaceID(), "mode");
        post_event(monitor->activeWorkspaceID(), "overview");
    });
    activeWindowHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "activeWindow", [&](void* /* self */, SCallbackInfo& /* info */, std::any param) {
        auto window = std::any_cast<PHLWINDOW>(param);
        trails->post_trailmark_event(window);
        marks.post_mark_event(window);
    });

    swipeBeginHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "swipeBegin", [&](void* /* self */, SCallbackInfo& /* info */, std::any param) {
        auto swipe_event = std::any_cast<IPointer::SSwipeBeginEvent>(param);
        swipe_begin(swipe_event);
    });

    swipeUpdateHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "swipeUpdate", [&](void* /* self */, SCallbackInfo& info, std::any param) {
        auto swipe_event = std::any_cast<IPointer::SSwipeUpdateEvent>(param);
        swipe_update(info, swipe_event);
    });

    swipeEndHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "swipeEnd", [&](void* /* self */, SCallbackInfo& info, std::any param) {
        auto swipe_event = std::any_cast<IPointer::SSwipeEndEvent>(param);
        swipe_end(info, swipe_event);
    });

    mouseMoveHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "mouseMove", [&](void* /* self */, SCallbackInfo& info, std::any param) {
        Vector2D mousePos = std::any_cast<Vector2D>(param);
        mouse_move(info, mousePos);
    });

    enabled = true;
    overviews = new Overview;
    marks.reset();
    trails = new Trails();
    for (auto& window : g_pCompositor->m_windows) {
        if (window->m_isFloating || !window->m_isMapped || window->isHidden())
            continue;

        onWindowCreatedTiling(window);
    }
    for (auto &monitor : g_pCompositor->m_monitors) {
        recalculateMonitor(monitor->m_id);
    }
}

void ScrollerLayout::onDisable() {
    // Restore Hyprland's default dispatchers
    g_pKeybindManager->m_dispatchers["movefocus"] = orig_moveFocusTo;
    g_pKeybindManager->m_dispatchers["movewindow"] = orig_moveActiveTo;

    // Unregister dynamic callbacks for events
    if (workspaceHookCallback != nullptr) {
        workspaceHookCallback.reset();
        workspaceHookCallback = nullptr;
    }
    if (focusedMonHookCallback != nullptr) {
        focusedMonHookCallback.reset();
        focusedMonHookCallback = nullptr;
    }
    if (activeWindowHookCallback != nullptr) {
        activeWindowHookCallback.reset();
        activeWindowHookCallback = nullptr;
    }
    if (swipeBeginHookCallback != nullptr) {
        swipeBeginHookCallback.reset();
        swipeBeginHookCallback = nullptr;
    }
    if (swipeUpdateHookCallback != nullptr) {
        swipeUpdateHookCallback.reset();
        swipeUpdateHookCallback = nullptr;
    }
    if (swipeEndHookCallback != nullptr) {
        swipeEndHookCallback.reset();
        swipeEndHookCallback = nullptr;
    }
    if (mouseMoveHookCallback != nullptr) {
        mouseMoveHookCallback.reset();
        mouseMoveHookCallback = nullptr;
    }

    if (overviews != nullptr) {
        delete overviews;
        overviews = nullptr;
    }
    enabled = false;
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        delete row->data();
    }
    rows.clear();
    marks.reset();
    delete trails;
    trails = nullptr;
}

/*
    Called to predict the size of a newly opened window to send it a configure.
    Return 0,0 if unpredictable
*/
Vector2D ScrollerLayout::predictSizeForNewWindowTiled() {
    if (!g_pCompositor->m_lastMonitor)
        return {};

    WORKSPACEID workspace_id = g_pCompositor->m_lastMonitor->activeWorkspaceID();
    auto s = getRowForWorkspace(workspace_id);
    if (s == nullptr) {
        Vector2D size =g_pCompositor->m_lastMonitor->m_size;
        size.x *= 0.5;
        return size;
    }

    return s->predict_window_size();
}

void ScrollerLayout::cycle_window_size(WORKSPACEID workspace, int step)
{
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->resize_active_column(step);
}

void ScrollerLayout::cycle_window_width(WORKSPACEID workspace, int step)
{
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    Mode mode = s->get_mode();
    s->set_mode(Mode::Row, true);
    s->resize_active_column(step);
    s->set_mode(mode, true);
}

void ScrollerLayout::cycle_window_height(WORKSPACEID workspace, int step)
{
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    Mode mode = s->get_mode();
    s->set_mode(Mode::Column, true);
    s->resize_active_column(step);
    s->set_mode(mode, true);
}

void ScrollerLayout::set_window_size(WORKSPACEID workspace, const std::string &arg)
{
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->size_active_column(arg);
}

void ScrollerLayout::set_window_width(WORKSPACEID workspace, const std::string &arg)
{
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    Mode mode = s->get_mode();
    s->set_mode(Mode::Row, true);
    s->size_active_column(arg);
    s->set_mode(mode, true);
}

void ScrollerLayout::set_window_height(WORKSPACEID workspace, const std::string &arg)
{
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    Mode mode = s->get_mode();
    s->set_mode(Mode::Column, true);
    s->size_active_column(arg);
    s->set_mode(mode, true);
}

void ScrollerLayout::move_focus(WORKSPACEID workspace, Direction direction)
{
    static auto* const *focus_wrap = (Hyprlang::INT* const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:focus_wrap")->getDataStaticPtr();
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        // if workspace is empty, use the deault movefocus, which now
        // is "move to another monitor" (pass the direction)
        switch (direction) {
            case Direction::Left:
                orig_moveFocusTo("l");
                break;
            case Direction::Right:
                orig_moveFocusTo("r");
                break;
            case Direction::Up:
                orig_moveFocusTo("u");
                break;
            case Direction::Down:
                orig_moveFocusTo("d");
                break;
            default:
                break;
        }
        return;
    }

    auto from = s->get_active_window();
    update_relative_cursor_coords(from);

    if (s->move_focus(direction, **focus_wrap == 0 ? false : true)) {
        // Changed workspace
        WORKSPACEID workspace_id = g_pCompositor->m_lastMonitor->activeSpecialWorkspaceID();
        if (!workspace_id) {
            workspace_id = g_pCompositor->m_lastMonitor->activeWorkspaceID();
        }
        s = getRowForWorkspace(workspace_id);
        if (s != nullptr) {
            s->recalculate_row_geometry();
        }
    }
    PHLWINDOW to = s != nullptr ? s->get_active_window() : nullptr;
    switch_to_window(from, to);
}

void ScrollerLayout::move_window(WORKSPACEID workspace, Direction direction, bool nomode) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    if (nomode)
        s->move_active_window(direction);
    else
        s->move_active_column(direction);
}

void ScrollerLayout::align_window(WORKSPACEID workspace, Direction direction) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->align_column(direction);
}

void ScrollerLayout::admit_window(WORKSPACEID workspace, AdmitExpelDirection direction) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->admit_window(direction);
}

void ScrollerLayout::expel_window(WORKSPACEID workspace, AdmitExpelDirection direction) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->expel_window(direction);
}

void ScrollerLayout::set_mode(WORKSPACEID workspace, Mode mode) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->set_mode(mode);
}

void ScrollerLayout::set_mode_modifier(WORKSPACEID workspace, const ModeModifier &modifier) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->set_mode_modifier(modifier);
}

void ScrollerLayout::fit_size(WORKSPACEID workspace, FitSize fitsize) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->fit_size(fitsize);
}

void ScrollerLayout::fit_width(WORKSPACEID workspace, FitSize fitsize) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    Mode mode = s->get_mode();
    s->set_mode(Mode::Row, true);
    s->fit_size(fitsize);
    s->set_mode(mode, true);
}

void ScrollerLayout::fit_height(WORKSPACEID workspace, FitSize fitsize) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    Mode mode = s->get_mode();
    s->set_mode(Mode::Column, true);
    s->fit_size(fitsize);
    s->set_mode(mode, true);
}

void ScrollerLayout::toggle_overview(WORKSPACEID workspace) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }
    s->toggle_overview();
}

PHLWINDOW ScrollerLayout::getActiveWindow(WORKSPACEID workspace) {
    const Row *s = getRowForWorkspace(workspace);
    if (s == nullptr)
        return nullptr;

    return s->get_active_window();
}

void ScrollerLayout::marks_add(const std::string &name) {
    PHLWINDOW window = getActiveWindow(get_workspace_id());
    if (window != nullptr)
        marks.add(window, name);
}

void ScrollerLayout::marks_delete(const std::string &name) {
    marks.del(name);
}

void ScrollerLayout::marks_visit(const std::string &name) {
    PHLWINDOW from = getActiveWindow(get_workspace_id());
    update_relative_cursor_coords(from);
    PHLWINDOW to = marks.visit(name);
    if (to != nullptr) {
        switch_to_window(from, to);
    }
}

void ScrollerLayout::marks_reset() {
    marks.reset();
}

// Trails and Trailmarks
void ScrollerLayout::trail_new() {
    trails->trail_new();
}

void ScrollerLayout::trail_next() {
    trails->trail_next();
}

void ScrollerLayout::trail_prev() {
    trails->trail_prev();
}

void ScrollerLayout::trail_delete() {
    trails->trail_delete();
}

void ScrollerLayout::trail_clear() {
    trails->trail_clear();
}

void ScrollerLayout::trail_toselection() {
    trails->trail_toselection();
}

void ScrollerLayout::trailmark_toggle() {
    PHLWINDOW window = getActiveWindow(get_workspace_id());
    if (window != nullptr)
        trails->trailmark_toggle(window);
}

void ScrollerLayout::trailmark_next() {
    trails->trailmark_next();
    PHLWINDOW from = getActiveWindow(get_workspace_id());
    update_relative_cursor_coords(from);
    PHLWINDOW to = trails->get_active();
    if (to != nullptr) {
        switch_to_window(from, to);
    }
}

void ScrollerLayout::trailmark_prev() {
    trails->trailmark_prev();
    PHLWINDOW from = getActiveWindow(get_workspace_id());
    update_relative_cursor_coords(from);
    PHLWINDOW to = trails->get_active();
    if (to != nullptr) {
        switch_to_window(from, to);
    }
}

void ScrollerLayout::pin(WORKSPACEID workspace) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->pin();
}

void ScrollerLayout::selection_toggle(WORKSPACEID workspace) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->selection_toggle();

    // Re-render that monitor to remove decorations
    g_pHyprRenderer->damageMonitor(g_pCompositor->m_lastMonitor.lock());
}

void ScrollerLayout::selection_set(PHLWINDOWREF window) {
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        row->data()->selection_set(window);
    }
}

void ScrollerLayout::selection_reset() {
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        row->data()->selection_reset();
    }
    // Re-render windows to remove decorations
    for (auto monitor : g_pCompositor->m_monitors) {
        g_pHyprRenderer->damageMonitor(monitor);
    }
}

void ScrollerLayout::selection_workspace(WORKSPACEID workspace) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->selection_all();

    // Re-render that monitor to render decorations
    g_pHyprRenderer->damageMonitor(g_pCompositor->m_lastMonitor.lock());
}

// Move all selected columns/windows to workspace, and locate them in direction wrt
// the active column. Valid directiona are left, right, beginning, end, other
// defaults to right.
void ScrollerLayout::selection_move(WORKSPACEID workspace, Direction direction) {
    // Before doing anything complicated, first checkt if there is any selection active
    bool selection = false;
    for (auto row = rows.first(); row != nullptr; row = row->next()) {
        if (row->data()->selection_exists()) {
            selection = true;
            break;
        }
    }
    if (!selection)
        return;

    auto s = getRowForWorkspace(workspace);
    bool overview_on = false;
    if (s == nullptr) {
        s = new Row(workspace);
        rows.push_back(s);
    } else {
        overview_on = s->is_overview();
        if (overview_on)
            s->toggle_overview();
    }
    // First modify ScrollerLayout internal structures and then call
    // CWindow::moveToWorkspace(PHLWORKSPACE pWorkspace)
    // for each window, so Hyprland is aware of the changes.
    List<Column *> columns;
    auto row = rows.first();
    while (row != nullptr) {
        auto next = row->next();
        if (row->data()->size() > 0) {
            row->data()->selection_get(s, columns);
        }
        row = next;
    }

    s->selection_move(columns, direction);

    // Now delete those rows that may have become empty,
    // and recalculate the rest
    row = rows.first();
    while (row != nullptr) {
        auto next = row->next();
        if (row->data()->size() == 0) {
            rows.erase(row);
            delete row->data();
        } else {
            bool overview = row->data()->is_overview();
            if (overview)
                row->data()->toggle_overview();
            g_pCompositor->focusWindow(row->data()->get_active_window());
            row->data()->recalculate_row_geometry();
            if (overview)
                row->data()->toggle_overview();
        }
        row = next;
    }

    g_pCompositor->focusWindow(s->get_active_window());
    // Reset selection
    selection_reset();

    if (overview_on)
        s->toggle_overview();
}

typedef struct JumpData {
    typedef struct {
        Row *row;
        bool overview;
    } Rows;
    PHLWINDOWREF from_window;
    PHLMONITORREF from_monitor;
    std::vector<Rows> workspaces;
    std::vector<PHLWINDOWREF> windows;
    std::vector<JumpDecoration *> decorations;
    std::string keys;
    int keys_pressed = 0;
    int nkeys;
    unsigned int window_number = 0;
    SP<HOOK_CALLBACK_FN> keyPressHookCallback;
} JumpData;

static JumpData *jump_data;

static std::string generate_label(unsigned int i, const std::string &keys, unsigned int nkeys)
{
    size_t ksize = keys.size();
    std::string label;
    for (unsigned int n = 0, div = i; n < nkeys; ++n) {
        unsigned int rem = div % ksize;
        label.insert(0, &keys[rem], 1);
        div = div / ksize;
    }
    return label;
}

void ScrollerLayout::jump() {
    if (jumping)
        return;

    jumping = true;
    jump_data = new JumpData;

    for (auto monitor : g_pCompositor->m_monitors) {
        WORKSPACEID workspace_id = monitor->activeSpecialWorkspaceID();
        if (!workspace_id) {
            workspace_id = monitor->activeWorkspaceID();
        }
        auto s = getRowForWorkspace(workspace_id);
        if (s == nullptr)
            continue;

        jump_data->workspaces.push_back({s, s->is_overview()});
    }
    if (jump_data->workspaces.size() == 0) {
        delete jump_data;
        jumping = false;
        return;
    }

    for (auto workspace : jump_data->workspaces) {
        workspace.row->get_windows(jump_data->windows);
    }
    if (jump_data->windows.size() == 0) {
        delete jump_data;
        jumping = false;
        return;
    }

    static auto const *KEYS = (Hyprlang::STRING const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:jump_labels_keys")->getDataStaticPtr();
    jump_data->keys = *KEYS;
    jump_data->from_window = g_pCompositor->m_lastWindow;
    jump_data->from_monitor = g_pCompositor->m_lastMonitor;

    if (jump_data->keys.size() == 1 && jump_data->windows.size() > 1) {
        delete jump_data;
        jumping = false;
        return;
    }
    if (jump_data->windows.size() == 1)
        jump_data->nkeys = 1;
    else
        jump_data->nkeys = std::ceil(std::log10(jump_data->windows.size()) / std::log10(jump_data->keys.size()));

    // Set overview mode for those workspaces that are not
    for (auto workspace : jump_data->workspaces) {
        if (!workspace.overview) {
            workspace.row->toggle_overview();
        }
    }

    // Set decorations (in overview mode)
    int i = 0;
    for (auto window : jump_data->windows) {
        const std::string label = generate_label(i++, jump_data->keys, jump_data->nkeys);
        auto deco = makeUnique<JumpDecoration>(window.lock(), label);
        jump_data->decorations.push_back(deco.get());
        HyprlandAPI::addWindowDecoration(PHANDLE, window.lock(), std::move(deco));
    }

    jump_data->keys_pressed = 0;
    jump_data->window_number = 0;

    jump_data->keyPressHookCallback = HyprlandAPI::registerCallbackDynamic(PHANDLE, "keyPress", [&](void* /* self */, SCallbackInfo& info, std::any param) {
        auto keypress_event = std::any_cast<std::unordered_map<std::string, std::any>>(param);
        auto keyboard = std::any_cast<SP<IKeyboard>>(keypress_event["keyboard"]);
        auto event = std::any_cast<IKeyboard::SKeyEvent>(keypress_event["event"]);

        const auto KEYCODE = event.keycode + 8; // Because to xkbcommon it's +8 from libinput
        const xkb_keysym_t keysym = xkb_state_key_get_one_sym(keyboard->m_xkbState, KEYCODE);

        if (event.state != WL_KEYBOARD_KEY_STATE_PRESSED)
            return;

        // Check if key is valid, otherwise exit
        bool valid = false;
        for (int i = 0; i < jump_data->keys.size(); ++i) {
            std::string keyname(1, jump_data->keys[i]);
            xkb_keysym_t key = xkb_keysym_from_name(keyname.c_str(), XKB_KEYSYM_NO_FLAGS);
            if (key && key == keysym) {
                jump_data->window_number = jump_data->window_number * jump_data->keys.size() + i;
                valid = true;
                break;
            }
        }
        bool focus = false;
        if (valid) {
            jump_data->keys_pressed++;
            if (jump_data->keys_pressed == jump_data->nkeys) {
                if (jump_data->window_number < jump_data->windows.size())
                    focus = true;
            } else {
                info.cancelled = true;
                return;
            }
        }

        // Finished, remove decorations
        for (size_t i = 0; i < jump_data->windows.size(); ++i) {
            jump_data->windows[i]->removeWindowDeco(jump_data->decorations[i]);
        }

        // Restore original overview
        for (auto workspace : jump_data->workspaces) {
            if (!workspace.overview) {
                workspace.row->toggle_overview();
            }
        }
        if (focus) {
            update_relative_cursor_coords(jump_data->from_window.lock());
            switch_to_window(jump_data->from_window.lock(),
                             jump_data->windows[jump_data->window_number].lock());
        } else {
            if (jump_data->from_window != nullptr)
                jump_data->from_window->warpCursor();
            else {
                g_pCompositor->warpCursorTo(jump_data->from_monitor.lock()->middle());
                g_pCompositor->setActiveMonitor(jump_data->from_monitor.lock());
            }
        }
        info.cancelled = true;
        jump_data->keyPressHookCallback.reset();
        delete jump_data;
        jumping = false;
    });
}

void ScrollerLayout::post_event(WORKSPACEID workspace, const std::string &event) {
    auto s = getRowForWorkspace(workspace);
    if (s == nullptr) {
        return;
    }

    s->post_event(event);
}

void ScrollerLayout::swipe_begin(IPointer::SSwipeBeginEvent /* swipe_event */) {
    WORKSPACEID wid = get_workspace_id();
    if (wid == -1) {
        return;
    }

    swipe_active = false;
    swipe_direction = Direction::Begin;
}

void ScrollerLayout::swipe_update(SCallbackInfo &info, IPointer::SSwipeUpdateEvent swipe_event) {
    WORKSPACEID wid = get_workspace_id();
    if (wid == -1) {
        return;
    }

    auto s = getRowForWorkspace(wid);

    static auto *const *HS = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "gestures:workspace_swipe")->getDataStaticPtr();
    static auto *const *HSFINGERS = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "gestures:workspace_swipe_fingers")->getDataStaticPtr();
    static auto *const *HSFINGERSMIN = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "gestures:workspace_swipe_min_fingers")->getDataStaticPtr();
    static auto *const *NATURAL = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "input:touchpad:natural_scroll")->getDataStaticPtr();
    static auto *const *HSINVERT = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "gestures:workspace_swipe_invert")->getDataStaticPtr();
    static auto *const *GSENS = (Hyprlang::FLOAT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_sensitivity")->getDataStaticPtr();
    static auto *const *SENABLE = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_scroll_enable")->getDataStaticPtr();
    static auto *const *SFINGERS = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_scroll_fingers")->getDataStaticPtr();
    static auto *const *OENABLE = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_overview_enable")->getDataStaticPtr();
    static auto *const *OFINGERS = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_overview_fingers")->getDataStaticPtr();
    static auto *const *ODISTANCE = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_overview_distance")->getDataStaticPtr();
    static auto *const *WENABLE = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_workspace_switch_enable")->getDataStaticPtr();
    static auto *const *WFINGERS = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_workspace_switch_fingers")->getDataStaticPtr();
    static auto *const *WDISTANCE = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_workspace_switch_distance")->getDataStaticPtr();
    static auto const *WPREFIX = (Hyprlang::STRING const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:gesture_workspace_switch_prefix")->getDataStaticPtr();

    if (**HS &&
        (**HSFINGERS == swipe_event.fingers ||
         (**HSFINGERSMIN && swipe_event.fingers >= **HSFINGERS))) {
        info.cancelled = true; 
        return;
    }

    if (!(**SENABLE && swipe_event.fingers == **SFINGERS) &&
        !(**OENABLE && swipe_event.fingers == **OFINGERS) &&
        !(**WENABLE && swipe_event.fingers == **WFINGERS)) {
        return;
    }

    info.cancelled = true;
    Vector2D delta = swipe_event.delta;
    delta *= **NATURAL ? **GSENS : -**GSENS;
    if (!swipe_active) {
        gesture_delta = Vector2D(0.0, 0.0);
    }
    gesture_delta += delta;

    if (**SENABLE && swipe_event.fingers == **SFINGERS) {
        if (s == nullptr)
            return;
        if (std::abs(gesture_delta.x) > std::abs(gesture_delta.y))
            swipe_direction = gesture_delta.x > 0 ? Direction::Right : Direction::Left;
        else
            swipe_direction = gesture_delta.y > 0 ? Direction::Down : Direction::Up;
        s->scroll_update(swipe_direction, delta);
    } else {
        // Undo natural
        const Vector2D delta = gesture_delta * (**NATURAL ? -1.0 : 1.0);
        if (**OENABLE && swipe_event.fingers == **OFINGERS) {
            // Only accept the first update: one swipe, one trigger.
            if (swipe_active)
                return;
            if (delta.y <= -**ODISTANCE) {
                if (s == nullptr)
                    return;
                if (!s->is_overview()) {
                    s->toggle_overview();
                }
            } else if (delta.y >= **ODISTANCE) {
                if (s == nullptr)
                    return;
                if (s->is_overview()) {
                    s->toggle_overview();
                }
            }
        }
        if (**WENABLE && swipe_event.fingers == **WFINGERS) {
            // Only accept the first update: one swipe, one trigger.
            if (swipe_active)
                return;
            if (delta.x <= -**WDISTANCE) {
                std::string offset(*WPREFIX);
                g_pKeybindManager->m_dispatchers["workspace"](**HSINVERT ? offset + "+1" : offset + "-1");
            } else if (delta.x >= **WDISTANCE) {
                std::string offset(*WPREFIX);
                g_pKeybindManager->m_dispatchers["workspace"](**HSINVERT ? offset + "-1" : offset + "+1");
            }
        }
    }
    swipe_active = true;
}

void ScrollerLayout::swipe_end(SCallbackInfo &info,
                               IPointer::SSwipeEndEvent /* swipe_event */) {
    WORKSPACEID wid = get_workspace_id();
    if (wid == -1) {
        return;
    }
    // Only if scrolling
    if (swipe_direction != Direction::Begin) {
        auto s = getRowForWorkspace(wid);
        s->scroll_end(swipe_direction);
    }

    swipe_active = false;
    gesture_delta = Vector2D(0.0, 0.0);
    swipe_direction = Direction::Begin;
    info.cancelled = true;
}

void ScrollerLayout::mouse_move(SCallbackInfo& info, const Vector2D &mousePos) {
    static bool inside = false;
    auto PMONITOR = g_pCompositor->getMonitorFromVector(mousePos);
    WORKSPACEID workspace_id = PMONITOR->activeWorkspaceID();
    auto s = getRowForWorkspace(workspace_id);
    if (s != nullptr) {
        Box box = { PMONITOR->m_position + PMONITOR->m_reservedTopLeft,
                    PMONITOR->m_size - PMONITOR->m_reservedTopLeft - PMONITOR->m_reservedBottomRight};

        if (!s->get_max().contains_point(mousePos) && box.contains_point(mousePos)) {
            // We are in gaps_out territory
            static auto *const *TIMEOUT = (Hyprlang::INT *const *)HyprlandAPI::getConfigValue(PHANDLE, "plugin:scroller:focus_edge_ms")->getDataStaticPtr();
            static auto enteredTime = std::chrono::high_resolution_clock::now();
            auto eventTime = std::chrono::high_resolution_clock::now();
            if (!inside) {
                inside = true;
                enteredTime = eventTime;
                info.cancelled = true;
                return;
            } else {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(eventTime - enteredTime).count() < **TIMEOUT) {
                    info.cancelled = true;
                    return;
                }
            }
        }
    }
    inside = false;
}
