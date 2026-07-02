// ==WindhawkMod==
// @id              island-taskbar
// @name            Island Taskbar Hider
// @description     Hides Windows 11 taskbar tray elements (clock, notification bell, battery, volume, network) so the Island Command Center overlay can own them. Minimal, safe-for-explorer companion mod.
// @version         1.0.0
// @author          Matt Pincoski
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lversion
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Island Taskbar Hider

A deliberately minimal companion to the **Island Command Center** overlay mod.
This mod runs inside `explorer.exe` and hides native taskbar tray elements so
the island can present its own clock, notifications, and status indicators.

Unlike the overlay mod, this file contains **no** Direct2D, no worker threads,
and no networking. Everything it does happens on the taskbar's own UI thread
through the XAML visual tree, plus one registry write for the clock. This keeps
it safe to inject directly into `explorer.exe`.

It hides:
- **Clock & date** (via the official `ShowClock` setting)
- **Notification / Control Center bell**
- **Battery**, **Volume**, and **Network** system-tray icons

## Conflict note
Do **not** also enable the standalone *Taskbar tray system icon tweaks* mod for
the same icons: both hook the same `SystemTray.IconView` constructor, which is a
double-hook hazard. Use one or the other.

The XAML icon hiding (bell/battery/volume/network) is ported from the proven
*Taskbar tray system icon tweaks* mod by m417z, whose selectors are known-good
on Windows 11 build 26200.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- HideClock: true
  $name: Hide clock & date
  $description: >-
    Hides the native taskbar clock/date via the official Windows ShowClock
    setting. May require an Explorer refresh to fully take effect.
- HideBell: true
  $name: Hide notification bell
  $description: Hides the Notification / Control Center bell button.
- HideBattery: true
  $name: Hide battery icon
- HideVolume: true
  $name: Hide volume icon
- HideNetwork: true
  $name: Hide network icon
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <atomic>
#include <functional>
#include <list>
#include <string>

#undef GetCurrentTime

#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

using namespace winrt::Windows::UI::Xaml;

// ---------------------------------------------------------------------------
// Settings (no side-effectful global constructors: only PODs / atomics /
// empty weak_refs live at file scope; all real init happens in Wh_ModInit).
// ---------------------------------------------------------------------------
struct {
    bool hideClock;
    bool hideBell;
    bool hideBattery;
    bool hideVolume;
    bool hideNetwork;
} g_settings;

std::atomic<bool> g_systemTrayModuleHooked;
std::atomic<bool> g_unloading;

using FrameworkElementLoadedEventRevoker = winrt::impl::event_revoker<
    IFrameworkElement,
    &winrt::impl::abi<IFrameworkElement>::type::remove_Loaded>;

std::list<FrameworkElementLoadedEventRevoker> g_autoRevokerList;

// Bell re-apply state: the bell icon's content is recreated when its state
// changes, so we listen on its AutomationProperties::Name and re-apply.
winrt::weak_ref<FrameworkElement> g_bellSystemTrayIconElement;
int64_t g_bellAutomationNameChangedToken;

// ---------------------------------------------------------------------------
// Taskbar window / XAML root plumbing (ported verbatim from
// taskbar-tray-system-icon-tweaks.wh.cpp — known-good on build 26200).
// ---------------------------------------------------------------------------
HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

// ---------------------------------------------------------------------------
// Visual-tree walk helpers (ported verbatim).
// ---------------------------------------------------------------------------
FrameworkElement EnumParentElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback) {
    auto parent = element;
    while (true) {
        parent = Media::VisualTreeHelper::GetParent(parent)
                     .try_as<FrameworkElement>();
        if (!parent) {
            return nullptr;
        }

        if (enumCallback(parent)) {
            return parent;
        }
    }
}

FrameworkElement GetParentElementByName(FrameworkElement element, PCWSTR name) {
    return EnumParentElements(element, [name](FrameworkElement parent) {
        return parent.Name() == name;
    });
}

bool IsChildOfElementByName(FrameworkElement element, PCWSTR name) {
    return !!GetParentElementByName(element, name);
}

FrameworkElement EnumChildElements(
    FrameworkElement element,
    std::function<bool(FrameworkElement)> enumCallback) {
    int childrenCount = Media::VisualTreeHelper::GetChildrenCount(element);

    for (int i = 0; i < childrenCount; i++) {
        auto child = Media::VisualTreeHelper::GetChild(element, i)
                         .try_as<FrameworkElement>();
        if (!child) {
            Wh_Log(L"Failed to get child %d of %d", i + 1, childrenCount);
            continue;
        }

        if (enumCallback(child)) {
            return child;
        }
    }

    return nullptr;
}

FrameworkElement FindChildByName(FrameworkElement element, PCWSTR name) {
    return EnumChildElements(element, [name](FrameworkElement child) {
        return child.Name() == name;
    });
}

FrameworkElement FindChildByClassName(FrameworkElement element,
                                      PCWSTR className) {
    return EnumChildElements(element, [className](FrameworkElement child) {
        return winrt::get_class_name(child) == className;
    });
}

// https://stackoverflow.com/a/3382894
std::wstring StringToHex(std::wstring_view input) {
    static const WCHAR kHexDigits[] = L"0123456789ABCDEF";

    std::wstring output;
    output.reserve(input.length() * 5);
    for (WCHAR c : input) {
        output.push_back(kHexDigits[(c >> 12) & 15]);
        output.push_back(kHexDigits[(c >> 8) & 15]);
        output.push_back(kHexDigits[(c >> 4) & 15]);
        output.push_back(kHexDigits[c & 15]);
        output.push_back(L' ');
    }

    if (!output.empty() && output.back() == L' ') {
        output.resize(output.length() - 1);
    }

    return output;
}

// ---------------------------------------------------------------------------
// Glyph-based icon identification (subset of the reference's enum + table,
// limited to the four icons this mod targets: volume, network, battery, bell).
// The glyph code points are copied verbatim from the reference so they stay
// known-correct for build 26200.
// ---------------------------------------------------------------------------
enum class SystemTrayIconIdent {
    kUnknown,
    kNone,
    kVolume,
    kNetwork,
    kBattery,
    kBellEmpty,
    kBellEmptyDnd,
    kBellFull,
    kBellFullDnd,
};

SystemTrayIconIdent IdentifySystemTrayIconFromText(std::wstring_view text) {
    switch (text.length()) {
        case 0:
            return SystemTrayIconIdent::kNone;

        case 1:
            break;

        default:
            return SystemTrayIconIdent::kUnknown;
    }

    switch (text[0]) {
        case L'':  // Mute
        case L'':  // Volume0
        case L'':  // Volume1
        case L'':  // Volume2
        case L'':  // Volume3
        case L'':  // VolumeDisabled
        case L'':  // VolumeBars
            return SystemTrayIconIdent::kVolume;

        case L'':  // Airplane
        case L'':  // TVMonitor
        case L'':  // Ethernet
        case L'':  // SignalBars1
        case L'':  // SignalBars2
        case L'':  // SignalBars3
        case L'':  // SignalBars4
        case L'':  // SignalBars5
        case L'':  // Drinks
        case L'':  // DropShot
        case L'':  // FlavorProfile
        case L'':  // Float
        case L'':  // FluteGlass
        case L'':  // GobletGlass
        case L'':  // SignalRoaming
        case L'':  // MobWifi1
        case L'':  // MobWifi2
        case L'':  // MobWifi3
        case L'':  // MobWifi4
        case L'':  // NetworkOffline
        case L'':  // SysLocationArrow
        case L'':  // SysMicrophone
        case L'':  // SysVideo
        case L'':  // SysWifi4
        case L'':  // SysWifi3
        case L'':  // SysWifi2
        case L'':  // SysWifi1
        case L'':  // SysSignalBars5
        case L'':  // SysSignalBars4
        case L'':  // SysSignalBars3
        case L'':  // SysSignalBars2
        case L'':  // SysSignalBars1
        case L'':  // SysNetworkOffline
            return SystemTrayIconIdent::kNetwork;

        // Charging levels.
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // Private Use
        case L'':  // MobBattery0
        case L'':  // MobBattery1
        case L'':  // MobBattery2
        case L'':  // MobBattery3
        case L'':  // MobBattery4
        case L'':  // MobBattery5
        case L'':  // MobBattery6
        case L'':  // MobBattery7
        case L'':  // MobBattery8
        case L'':  // MobBattery9
        case L'':  // MobBattery10
        case L'':  // MobBatteryCharging0
        case L'':  // MobBatteryCharging1
        case L'':  // MobBatteryCharging2
        case L'':  // MobBatteryCharging3
        case L'':  // MobBatteryCharging4
        case L'':  // MobBatteryCharging5
        case L'':  // MobBatteryCharging6
        case L'':  // MobBatteryCharging7
        case L'':  // MobBatteryCharging8
        case L'':  // MobBatteryCharging9
        case L'':  // MobBatteryCharging10
        case L'':  // MobBatterySaver0
        case L'':  // MobBatterySaver1
        case L'':  // MobBatterySaver2
        case L'':  // MobBatterySaver3
        case L'':  // MobBatterySaver4
        case L'':  // MobBatterySaver5
        case L'':  // MobBatterySaver6
        case L'':  // MobBatterySaver7
        case L'':  // MobBatterySaver8
        case L'':  // MobBatterySaver9
        case L'':  // MobBatterySaver10
        // Misc.
        case L'':
        case L'':
        case L'':
            return SystemTrayIconIdent::kBattery;

        case L'':  // Empty bell
            return SystemTrayIconIdent::kBellEmpty;

        case L'':  // Empty bell, Do Not Disturb
            return SystemTrayIconIdent::kBellEmptyDnd;

        case L'':  // Full bell
            return SystemTrayIconIdent::kBellFull;

        case L'':  // Full bell, Do Not Disturb
            return SystemTrayIconIdent::kBellFullDnd;
    }

    return SystemTrayIconIdent::kUnknown;
}

// ---------------------------------------------------------------------------
// Per-icon apply functions. Each is wrapped so no WinRT exception can escape
// into explorer's XAML dispatcher.
// ---------------------------------------------------------------------------

// Handles volume / network / battery, which on build 26200 live under
// ControlCenterButton. Ported from ApplyControlCenterButtonIconStyle, reduced
// to hide-only (no grayscale, no cross-icon container collapse).
void ApplyControlCenterButtonIconStyle(FrameworkElement systemTrayIconElement) {
    try {
        FrameworkElement contentGrid = nullptr;

        FrameworkElement child = systemTrayIconElement;
        if ((child = FindChildByName(child, L"ContainerGrid")) &&
            (child = FindChildByName(child, L"ContentGrid"))) {
            contentGrid = child;
        } else {
            Wh_Log(L"Failed to get ContentGrid");
            return;
        }

        bool hide = false;
        FrameworkElement systemTrayTextIconContent =
            FindChildByClassName(contentGrid, L"SystemTray.BatteryIconContent");
        if (systemTrayTextIconContent) {
            if (!g_unloading) {
                hide = g_settings.hideBattery;
            }
            Wh_Log(L"System battery tray icon, hide=%d", hide);
        } else {
            systemTrayTextIconContent = FindChildByClassName(
                contentGrid, L"SystemTray.TextIconContent");
            if (!systemTrayTextIconContent) {
                Wh_Log(L"Failed to get SystemTray.TextIconContent");
                return;
            }

            Controls::TextBlock innerTextBlock = nullptr;

            child = systemTrayTextIconContent;
            if ((child = FindChildByName(child, L"ContainerGrid")) &&
                (child = FindChildByName(child, L"Base")) &&
                (child = FindChildByName(child, L"InnerTextBlock"))) {
                innerTextBlock = child.try_as<Controls::TextBlock>();
            }
            if (!innerTextBlock) {
                Wh_Log(L"Failed to get InnerTextBlock");
                return;
            }

            auto text = innerTextBlock.Text();
            auto systemTrayIconIdent = IdentifySystemTrayIconFromText(text);

            if (!g_unloading) {
                switch (systemTrayIconIdent) {
                    case SystemTrayIconIdent::kVolume:
                        hide = g_settings.hideVolume;
                        break;

                    case SystemTrayIconIdent::kNetwork:
                        hide = g_settings.hideNetwork;
                        break;

                    case SystemTrayIconIdent::kBattery:
                        hide = g_settings.hideBattery;
                        break;

                    default:
                        // Not one of our targets (e.g. an icon this mod does
                        // not manage); leave it visible.
                        break;
                }
            }

            Wh_Log(L"System tray icon %d (%s), hide=%d",
                   (int)systemTrayIconIdent, StringToHex(text).c_str(), hide);
        }

        // Idempotent: only mutate on change to avoid relayout storms.
        bool hidden =
            systemTrayTextIconContent.Visibility() == Visibility::Collapsed;
        if (hide == hidden) {
            return;
        }

        systemTrayTextIconContent.Visibility(hide ? Visibility::Collapsed
                                                  : Visibility::Visible);
        if (auto control =
                systemTrayIconElement.try_as<Controls::Control>()) {
            control.IsEnabled(!hide);
        }
    } catch (const winrt::hresult_error& e) {
        Wh_Log(L"WinRT 0x%08X: %s", e.code().value, e.message().c_str());
    } catch (...) {
        Wh_Log(L"Unknown exception");
    }
}

void ApplyBellIconStyle(FrameworkElement systemTrayIconElement);

// When the clock is hidden, the bell icon's content is recreated each time the
// bell state changes; retry until the content is present. Ported verbatim
// (with try/catch guard).
void ApplyBellIconStyleWithRetry(FrameworkElement systemTrayIconElement,
                                 int attempt) {
    try {
        Wh_Log(L"> %d", attempt);

        if (attempt == 10) {
            return;
        }

        FrameworkElement containerGrid =
            FindChildByName(systemTrayIconElement, L"ContainerGrid");
        if (!containerGrid) {
            systemTrayIconElement.Dispatcher().TryRunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Low,
                [systemTrayIconElement, attempt]() {
                    ApplyBellIconStyleWithRetry(systemTrayIconElement,
                                                attempt + 1);
                });
            return;
        }

        ApplyBellIconStyle(systemTrayIconElement);
    } catch (const winrt::hresult_error& e) {
        Wh_Log(L"WinRT 0x%08X: %s", e.code().value, e.message().c_str());
    } catch (...) {
        Wh_Log(L"Unknown exception");
    }
}

// Bell / Notification-Center button. Ported from ApplyBellIconStyle, reduced to
// hide-always/restore semantics (no whenInactive glyph logic). Preserves the
// clock-hidden ContentPresenter/MaxWidth structural branch.
void ApplyBellIconStyle(FrameworkElement systemTrayIconElement) {
    try {
        FrameworkElement containerGrid =
            FindChildByName(systemTrayIconElement, L"ContainerGrid");
        if (!containerGrid) {
            Wh_Log(L"Failed to get ContainerGrid");
            return;
        }

        FrameworkElement systemTrayTextIconContent = nullptr;
        bool hasContentPresenterForMissingClock = true;

        // Clock hidden:  #ContainerGrid > #ContentPresenter > #ContentGrid >
        //                SystemTray.TextIconContent
        // Clock visible: #ContainerGrid > #ContentGrid >
        //                SystemTray.TextIconContent  (no ContentPresenter)
        FrameworkElement child =
            FindChildByName(containerGrid, L"ContentPresenter");
        if (!child) {
            hasContentPresenterForMissingClock = false;
            child = containerGrid;
        }

        if ((child = FindChildByName(child, L"ContentGrid")) &&
            (child =
                 FindChildByClassName(child, L"SystemTray.TextIconContent"))) {
            systemTrayTextIconContent = child;
        } else {
            // Content not populated yet; the retry path / Name callback will
            // bring us back.
            return;
        }

        auto contentPresenter =
            Media::VisualTreeHelper::GetParent(systemTrayIconElement)
                .try_as<FrameworkElement>();

        bool hide = !g_unloading && g_settings.hideBell;

        // Re-apply when the bell state changes (content gets recreated).
        if (!g_unloading && g_settings.hideBell &&
            !g_bellSystemTrayIconElement.get()) {
            g_bellSystemTrayIconElement = systemTrayIconElement;
            g_bellAutomationNameChangedToken =
                systemTrayIconElement.RegisterPropertyChangedCallback(
                    Automation::AutomationProperties::NameProperty(),
                    [](DependencyObject sender, DependencyProperty property) {
                        try {
                            auto bellSystemTrayIconElement =
                                sender.try_as<FrameworkElement>();
                            if (!bellSystemTrayIconElement) {
                                return;
                            }
                            ApplyBellIconStyleWithRetry(
                                bellSystemTrayIconElement, 0);
                        } catch (const winrt::hresult_error& e) {
                            Wh_Log(L"WinRT 0x%08X", e.code().value);
                        } catch (...) {
                            Wh_Log(L"Unknown exception");
                        }
                    });
        }

        Wh_Log(L"Bell icon, hide=%d", hide);

        systemTrayTextIconContent.Visibility(hide ? Visibility::Collapsed
                                                  : Visibility::Visible);

        if (contentPresenter) {
            if (hide && hasContentPresenterForMissingClock) {
                contentPresenter.MaxWidth(0);
            } else {
                contentPresenter.ClearValue(
                    FrameworkElement::MaxWidthProperty());
            }
        }
    } catch (const winrt::hresult_error& e) {
        Wh_Log(L"WinRT 0x%08X: %s", e.code().value, e.message().c_str());
    } catch (...) {
        Wh_Log(L"Unknown exception");
    }
}

// ---------------------------------------------------------------------------
// Full-tree apply for already-existing icons (mirrors the reference's
// ApplyStyle, restricted to ControlCenterButton + NotificationCenterButton).
// ---------------------------------------------------------------------------
bool ApplyButtonStyle(
    FrameworkElement controlCenterButton,
    const std::function<void(FrameworkElement)>& applyIcon) {
    FrameworkElement stackPanel = nullptr;

    FrameworkElement child = controlCenterButton;
    if ((child =
             FindChildByClassName(child, L"Windows.UI.Xaml.Controls.Grid")) &&
        (child = FindChildByName(child, L"ContentPresenter")) &&
        (child = FindChildByClassName(
             child, L"Windows.UI.Xaml.Controls.ItemsPresenter")) &&
        (child = FindChildByClassName(
             child, L"Windows.UI.Xaml.Controls.StackPanel"))) {
        stackPanel = child;
    }

    if (!stackPanel) {
        return false;
    }

    EnumChildElements(stackPanel, [&applyIcon](FrameworkElement child) {
        auto childClassName = winrt::get_class_name(child);
        if (childClassName != L"Windows.UI.Xaml.Controls.ContentPresenter") {
            Wh_Log(L"Unsupported class name %s of child",
                   childClassName.c_str());
            return false;
        }

        FrameworkElement systemTrayIconElement =
            FindChildByName(child, L"SystemTrayIcon");
        if (!systemTrayIconElement) {
            Wh_Log(L"Failed to get SystemTrayIcon of child");
            return false;
        }

        applyIcon(systemTrayIconElement);
        return false;
    });

    return true;
}

bool ApplyStyle(XamlRoot xamlRoot) {
    FrameworkElement systemTrayFrameGrid = nullptr;

    FrameworkElement child = xamlRoot.Content().try_as<FrameworkElement>();
    if (child &&
        (child = FindChildByClassName(child, L"SystemTray.SystemTrayFrame")) &&
        (child = FindChildByName(child, L"SystemTrayFrameGrid"))) {
        systemTrayFrameGrid = child;
    }

    if (!systemTrayFrameGrid) {
        return false;
    }

    bool somethingSucceeded = false;

    FrameworkElement controlCenterButton =
        FindChildByName(systemTrayFrameGrid, L"ControlCenterButton");
    if (controlCenterButton) {
        somethingSucceeded |=
            ApplyButtonStyle(controlCenterButton,
                             ApplyControlCenterButtonIconStyle);
    }

    FrameworkElement notificationCenterButton =
        FindChildByName(systemTrayFrameGrid, L"NotificationCenterButton");
    if (notificationCenterButton) {
        somethingSucceeded |=
            ApplyButtonStyle(notificationCenterButton, ApplyBellIconStyle);
    }

    return somethingSucceeded;
}

// ---------------------------------------------------------------------------
// IconView constructor hook — catches newly created icons and applies on Loaded.
// Symbol string, ABI extraction, and revoker pattern are ported verbatim.
// ---------------------------------------------------------------------------
using IconView_IconView_t = void*(WINAPI*)(void* pThis);
IconView_IconView_t IconView_IconView_Original;
void* WINAPI IconView_IconView_Hook(void* pThis) {
    Wh_Log(L">");

    void* ret = IconView_IconView_Original(pThis);

    try {
        FrameworkElement iconView = nullptr;
        ((IUnknown**)pThis)[1]->QueryInterface(
            winrt::guid_of<FrameworkElement>(), winrt::put_abi(iconView));
        if (!iconView) {
            return ret;
        }

        g_autoRevokerList.emplace_back();
        auto autoRevokerIt = g_autoRevokerList.end();
        --autoRevokerIt;

        *autoRevokerIt = iconView.Loaded(
            winrt::auto_revoke_t{},
            [autoRevokerIt](
                winrt::Windows::Foundation::IInspectable const& sender,
                RoutedEventArgs const& e) {
                try {
                    Wh_Log(L">");

                    g_autoRevokerList.erase(autoRevokerIt);

                    auto iconView = sender.try_as<FrameworkElement>();
                    if (!iconView) {
                        return;
                    }

                    auto className = winrt::get_class_name(iconView);
                    Wh_Log(L"className: %s", className.c_str());

                    if (className == L"SystemTray.IconView" &&
                        iconView.Name() == L"SystemTrayIcon") {
                        if (IsChildOfElementByName(iconView,
                                                   L"ControlCenterButton")) {
                            ApplyControlCenterButtonIconStyle(iconView);
                        } else if (IsChildOfElementByName(
                                       iconView,
                                       L"NotificationCenterButton")) {
                            ApplyBellIconStyle(iconView);
                        }
                    }
                } catch (const winrt::hresult_error& e) {
                    Wh_Log(L"WinRT 0x%08X", e.code().value);
                } catch (...) {
                    Wh_Log(L"Unknown exception");
                }
            });
    } catch (const winrt::hresult_error& e) {
        Wh_Log(L"WinRT 0x%08X", e.code().value);
    } catch (...) {
        Wh_Log(L"Unknown exception");
    }

    return ret;
}

// ---------------------------------------------------------------------------
// taskbar.dll symbols needed by GetTaskbarXamlRoot (ported verbatim).
// ---------------------------------------------------------------------------
void* CTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;

void* TaskbarHost_FrameHeight_Original;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

XamlRoot GetTaskbarXamlRoot(HWND hTaskbarWnd) {
    HWND hTaskSwWnd = (HWND)GetProp(hTaskbarWnd, L"TaskbandHWND");
    if (!hTaskSwWnd) {
        return nullptr;
    }

    void* taskBand = (void*)GetWindowLongPtr(hTaskSwWnd, 0);
    void* taskBandForTaskListWndSite = taskBand;
    for (int i = 0; *(void**)taskBandForTaskListWndSite !=
                    CTaskBand_ITaskListWndSite_vftable;
         i++) {
        if (i == 20) {
            return nullptr;
        }

        taskBandForTaskListWndSite = (void**)taskBandForTaskListWndSite + 1;
    }

    void* taskbarHostSharedPtr[2]{};
    CTaskBand_GetTaskbarHost_Original(taskBandForTaskListWndSite,
                                      taskbarHostSharedPtr);
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1]) {
        return nullptr;
    }

    size_t taskbarElementIUnknownOffset = 0x48;

#if defined(_M_X64)
    {
        // 48:83EC 28 | sub rsp,28
        // 48:83C1 48 | add rcx,48
        const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 &&
            b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F) {
            taskbarElementIUnknownOffset = b[7];
        } else {
            Wh_Log(L"Unsupported TaskbarHost::FrameHeight");
        }
    }
#elif defined(_M_ARM64)
    // Just use the default offset which will hopefully work in most cases.
#else
#error "Unsupported architecture"
#endif

    auto* taskbarElementIUnknown =
        *(IUnknown**)((BYTE*)taskbarHostSharedPtr[0] +
                      taskbarElementIUnknownOffset);

    FrameworkElement taskbarElement = nullptr;
    taskbarElementIUnknown->QueryInterface(winrt::guid_of<FrameworkElement>(),
                                           winrt::put_abi(taskbarElement));

    auto result = taskbarElement ? taskbarElement.XamlRoot() : nullptr;

    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);

    return result;
}

// ---------------------------------------------------------------------------
// RunFromWindowThread — marshal a proc onto the taskbar UI thread (verbatim).
// ---------------------------------------------------------------------------
using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

bool RunFromWindowThread(HWND hWnd,
                         RunFromWindowThreadProc_t proc,
                         void* procParam) {
    static const UINT runFromWindowThreadRegisteredMsg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    struct RUN_FROM_WINDOW_THREAD_PARAM {
        RunFromWindowThreadProc_t proc;
        void* procParam;
    };

    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION) {
                const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
                if (cwp->message == runFromWindowThreadRegisteredMsg) {
                    RUN_FROM_WINDOW_THREAD_PARAM* param =
                        (RUN_FROM_WINDOW_THREAD_PARAM*)cwp->lParam;
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RUN_FROM_WINDOW_THREAD_PARAM param;
    param.proc = proc;
    param.procParam = procParam;
    SendMessage(hWnd, runFromWindowThreadRegisteredMsg, 0, (LPARAM)&param);

    UnhookWindowsHookEx(hook);

    return true;
}

// ---------------------------------------------------------------------------
// Clock — official ShowClock setting + tray settings-change broadcast.
// ---------------------------------------------------------------------------
void ApplyClockRegistry(bool hide) {
    HKEY hKey;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        Wh_Log(L"Failed to open Advanced key");
        return;
    }

    DWORD value = hide ? 0 : 1;  // ShowClock: 0 hides, 1 shows.
    LSTATUS status = RegSetValueExW(hKey, L"ShowClock", 0, REG_DWORD,
                                    (const BYTE*)&value, sizeof(value));
    RegCloseKey(hKey);

    if (status != ERROR_SUCCESS) {
        Wh_Log(L"Failed to set ShowClock (%d)", (int)status);
        return;
    }

    // Best-effort nudge for the tray to re-read tray settings.
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 200,
                        nullptr);

    Wh_Log(L"ShowClock set to %d", (int)value);
}

// ---------------------------------------------------------------------------
// Settings + apply orchestration.
// ---------------------------------------------------------------------------
void LoadSettings() {
    g_settings.hideClock = Wh_GetIntSetting(L"HideClock");
    g_settings.hideBell = Wh_GetIntSetting(L"HideBell");
    g_settings.hideBattery = Wh_GetIntSetting(L"HideBattery");
    g_settings.hideVolume = Wh_GetIntSetting(L"HideVolume");
    g_settings.hideNetwork = Wh_GetIntSetting(L"HideNetwork");
}

void ApplySettings() {
    struct ApplySettingsParam {
        HWND hTaskbarWnd;
    };

    Wh_Log(L"Applying settings");

    // Clock is handled off the UI thread (registry + broadcast). When
    // unloading, always restore the clock (ShowClock=1).
    ApplyClockRegistry(!g_unloading && g_settings.hideClock);

    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        Wh_Log(L"No taskbar found");
        return;
    }

    ApplySettingsParam param{
        .hTaskbarWnd = hTaskbarWnd,
    };

    RunFromWindowThread(
        hTaskbarWnd,
        [](void* pParam) {
            ApplySettingsParam& param = *(ApplySettingsParam*)pParam;

            try {
                // Drop stale Loaded revokers.
                g_autoRevokerList.clear();

                // Unregister the bell Name callback; ApplyStyle re-registers if
                // still hiding.
                if (auto bellSystemTrayIconElement =
                        g_bellSystemTrayIconElement.get()) {
                    bellSystemTrayIconElement.UnregisterPropertyChangedCallback(
                        Automation::AutomationProperties::NameProperty(),
                        g_bellAutomationNameChangedToken);
                }
                g_bellSystemTrayIconElement = nullptr;
                g_bellAutomationNameChangedToken = 0;

                auto xamlRoot = GetTaskbarXamlRoot(param.hTaskbarWnd);
                if (!xamlRoot) {
                    Wh_Log(L"Getting XamlRoot failed");
                    return;
                }

                if (!ApplyStyle(xamlRoot)) {
                    Wh_Log(L"ApplyStyle failed");
                }
            } catch (const winrt::hresult_error& e) {
                Wh_Log(L"WinRT 0x%08X: %s", e.code().value,
                       e.message().c_str());
            } catch (...) {
                Wh_Log(L"Unknown exception");
            }
        },
        &param);
}

// ---------------------------------------------------------------------------
// Symbol hooking + module resolution (ported verbatim; -lversion required).
// ---------------------------------------------------------------------------
bool HookSystemTraySymbols(HMODULE module) {
    // SystemTray.dll, Taskbar.View.dll
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: __cdecl winrt::SystemTray::implementation::IconView::IconView(void))"},
            &IconView_IconView_Original,
            IconView_IconView_Hook,
        },
    };

    if (!HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks))) {
        Wh_Log(L"HookSymbols failed");
        return false;
    }

    return true;
}

VS_FIXEDFILEINFO* GetModuleVersionInfo(HMODULE hModule, UINT* puPtrLen) {
    void* pFixedFileInfo = nullptr;
    UINT uPtrLen = 0;

    HRSRC hResource =
        FindResource(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
    if (hResource) {
        HGLOBAL hGlobal = LoadResource(hModule, hResource);
        if (hGlobal) {
            void* pData = LockResource(hGlobal);
            if (pData) {
                if (!VerQueryValue(pData, L"\\", &pFixedFileInfo, &uPtrLen) ||
                    uPtrLen == 0) {
                    pFixedFileInfo = nullptr;
                    uPtrLen = 0;
                }
            }
        }
    }

    if (puPtrLen) {
        *puPtrLen = uPtrLen;
    }

    return (VS_FIXEDFILEINFO*)pFixedFileInfo;
}

// Returns the module that hosts winrt::SystemTray::* in the current build.
// Order matters: SystemTray.dll is the new home (Win11 Insider 26200+);
// Taskbar.View.dll is kept as a fallback so this still works on older builds.
HMODULE GetSystemTrayModuleHandle() {
    HMODULE module = GetModuleHandle(L"SystemTray.dll");
    if (!module) {
        module = GetModuleHandle(L"Taskbar.View.dll");
        if (module) {
            // First known module version without SystemTray is Taskbar.View.dll
            // 2604.8002.200.6000.
            VS_FIXEDFILEINFO* fixedFileInfo =
                GetModuleVersionInfo(module, nullptr);
            WORD moduleMajor =
                fixedFileInfo ? HIWORD(fixedFileInfo->dwFileVersionMS) : 0;
            if (!moduleMajor || moduleMajor >= 2604) {
                Wh_Log(L"Skipping Taskbar.View.dll version %d", moduleMajor);
                module = nullptr;
            }
        }
    }

    return module;
}

void HandleLoadedModuleIfSystemTray(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_systemTrayModuleHooked && GetSystemTrayModuleHandle() == module &&
        !g_systemTrayModuleHooked.exchange(true)) {
        Wh_Log(L"Loaded %s", lpLibFileName);

        if (HookSystemTraySymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModuleIfSystemTray(module, lpLibFileName);
    }

    return module;
}

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Failed to load taskbar.dll");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {
            {LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
            &CTaskBand_ITaskListWndSite_vftable,
        },
        {
            {LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
            &CTaskBand_GetTaskbarHost_Original,
        },
        {
            {LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
            &TaskbarHost_FrameHeight_Original,
        },
        {
            {LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
            &std__Ref_count_base__Decref_Original,
        },
    };

    return HookSymbols(module, taskbarDllHooks, ARRAYSIZE(taskbarDllHooks));
}

// ---------------------------------------------------------------------------
// Entry points.
// ---------------------------------------------------------------------------
BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    if (HMODULE systemTrayModule = GetSystemTrayModuleHandle()) {
        g_systemTrayModuleHooked = true;
        if (!HookSystemTraySymbols(systemTrayModule)) {
            return FALSE;
        }
    } else {
        Wh_Log(L"System tray module not loaded yet; deferring via LoadLibraryExW");

        HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
        auto pKernelBaseLoadLibraryExW =
            (decltype(&LoadLibraryExW))GetProcAddress(kernelBaseModule,
                                                      "LoadLibraryExW");
        WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                       LoadLibraryExW_Hook,
                                       &LoadLibraryExW_Original);
    }

    if (!HookTaskbarDllSymbols()) {
        return FALSE;
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    if (!g_systemTrayModuleHooked) {
        if (HMODULE systemTrayModule = GetSystemTrayModuleHandle()) {
            if (!g_systemTrayModuleHooked.exchange(true)) {
                Wh_Log(L"Got system tray module");

                if (HookSystemTraySymbols(systemTrayModule)) {
                    Wh_ApplyHookOperations();
                }
            }
        }
    }

    // Apply to icons that already exist (the ctor hook only catches new ones).
    ApplySettings();
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");

    // Restore EVERYTHING: all icons Visible, clock shown, callbacks cleared.
    g_unloading = true;

    ApplySettings();
}

void Wh_ModUninit() {
    Wh_Log(L">");
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();

    ApplySettings();
}
