# Compatibility reports

These are reports from setups actually used with Oh My 3DO. A working result
covers the listed setup and tasks; untested combinations still need reports.

## Console hardware

| Setup | Reported result | Known limits or quirks |
| --- | --- | --- |
| FCare/SataTo3DO ODE on the project's console | Browsing, game launching, covers, previews and return-to-library recovery tested on hardware | Reports `LOCODE 1.1.0`; exact firmware build and console model were not recorded. Keep Folder workaround on Auto and leave navigation helpers in place. |
| 8BitDo M30 through a BlueRetro 3DO adapter | Menu use reported | An initial button press may be needed before the menu responds. The cause has not been isolated. Hold P help needs about three quarters of a second; release after help opens. |

No model-specific storage or reader compatibility list has been established yet.
One SD extender caused an insert-disc screen during testing; its model was not
recorded. Check the card connection and extender if a prepared drive will not boot.

## Desktop app

| Platform | Reported result | Scope |
| --- | --- | --- |
| Windows | Installer, app launch and uninstallation reported working | Exact Windows build and app version were not recorded in that report. |
| Linux x64 | Native app interaction and updater/recovery checks passed in the development container | Desktop-distribution reports are welcome. |

These initial reports were collected before detailed compatibility tracking.
New reports should include versions and device models so the list becomes more useful.

## Share a result

Use the [compatibility report form](https://github.com/Scyllinice/oh-my-3do/issues/new?template=compatibility_report.yml).
Include versions, hardware models, tasks tried and any settings or quirks.
Unknown firmware details are fine; the diagnostics screen can show the reported identity.

For a reproducible problem, use the [bug report form](https://github.com/Scyllinice/oh-my-3do/issues/new?template=bug_report.yml).
The [troubleshooting guide](troubleshooting.md#collect-diagnostics) explains how
to collect diagnostics or a desktop support report.

[Project overview](../README.md)
