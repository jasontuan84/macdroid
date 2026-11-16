# Changelog

All notable changes to MacDroid are documented in this file.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

- Author: Jason Tuan
- Email: jasontuan.dev@gmail.com
- Website: https://tuanquynh.com

## [Unreleased]

### Fixed

- The browser window could not be moved. Its content was pushed below a
  32 px title bar "safe area", so drags in the title bar area never reached
  the toolbar or sidebar. The content now fills the whole window and window
  drags are handed to macOS (smooth, works across displays, supports edge
  tiling).
- Crash when a phone could not be opened (busy, held by Image Capture or a
  stale session): libmtp reset the USB device and libusb then crashed on the
  re-enumerated handle. MacDroid now skips that reset and shows the normal
  "phone is busy" screen instead.

## [1.0.0] - 2026-11-16

First release: an Android file browser for macOS 26 (Tahoe) and later that
talks to the phone over USB using MTP. Built with Qt 6.12 (Widgets) and a small
Objective-C++ layer for AppKit integration.

### Added

- **Menu bar extra**
  - Status icon with three states: no device (outline), action needed on the
    phone (exclamation) and connected (green phone with a check mark). While
    files are being copied, the green icon shows the progress.
  - Menu with device name, battery level, free space per storage, Open
    MacDroid, Quick Open (DCIM/Camera, Download, Pictures/Screenshots,
    Documents), running transfers, Eject, Settings and Quit.
  - Runs without a Dock icon; the Dock icon appears only while the browser
    window is open.
  - System notification when a phone connects and when a transfer finishes in
    the background.
- **Device detection**
  - USB hot-plug through IOKit, without claiming the device.
  - Recognises phones that are plugged in but not in "File transfer" mode
    (charging only, locked) and shows step-by-step instructions.
  - Detects when macOS Image Capture (`ptpcamerad`) holds the phone and offers
    a "Release Phone" action that hands the connection to MacDroid.
- **Browser window** (Finder-like, Liquid Glass style)
  - Floating glass sidebar with device, storages (free space and usage bar)
    and favourite folders (DCIM, Download, Pictures, Documents, Movies, Music).
  - Toolbar with back/forward, view mode, sort menu, New Folder, Copy to Mac,
    Delete, transfers indicator and search.
  - List view with Name, Date Modified, Size and Kind columns, and Grid view
    with large icons.
  - Folders are always listed before files; natural name sorting; sort by
    name, date modified, size or kind, ascending or descending.
  - Thumbnails in Grid view for images, videos (with play badge) and PDFs,
    generated lazily for visible items and cached on disk.
  - Native macOS file icons and localised "Kind" descriptions.
  - Path bar with clickable breadcrumbs and an item count / free space status.
  - Clean "Connect an Android phone" screen while no phone is connected.
- **File operations**
  - Double-click opens folders; files open in their default macOS app.
  - New Folder (with inline rename), Rename, Duplicate, Get Info.
  - Delete Permanently with confirmation (MTP has no Trash).
  - Copy (⌘C), Cut (⌘X), Paste (⌘V) and Move Item Here (⌥⌘V) on the phone.
  - Copy files from Finder and paste them into the phone.
  - Copy items on the phone and paste them in Finder or on the Desktop; files
    are downloaded only when actually pasted.
  - Drag & drop from Finder to the phone, from the phone to Finder/Desktop
    (file promises), and between folders on the phone (move, ⌥ to copy).
  - Copy to Mac… to a chosen folder.
  - Replace / Keep Both / Skip / Stop when names clash, with "Apply to all".
  - Transfers popover with progress, speed, time left and cancel.
- **Settings**
  - Appearance: System, Light or Dark, applied immediately.
  - Language: System default, English or Vietnamese.
  - Open the browser automatically when a phone connects.
  - Launch at Login.
  - Show hidden files.
- **Localisation**: English (default) and Vietnamese.

### Developer

- Debug builds support `--demo [grid]` (sample content, no phone needed),
  `--snapshot <png>` (render the window to an image) and `--selftest`
  (end-to-end MTP file operation test against a connected phone).
