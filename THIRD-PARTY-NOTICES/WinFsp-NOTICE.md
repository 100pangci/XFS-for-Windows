# WinFsp notice

XFS-for-Windows uses WinFsp for its native Windows filesystem mounting support.

- Product: WinFsp - Windows File System Proxy
- Copyright: Copyright (C) Bill Zissimopoulos
- Repository: https://github.com/winfsp/winfsp
- Pinned redistributable: `winfsp-2.1.25156.msi` (official release asset)
- License: GNU General Public License version 3 with the WinFsp FLOSS exception
- Full license text: [WinFsp-LICENSE.txt](WinFsp-LICENSE.txt), fetched from
  https://github.com/winfsp/winfsp/raw/refs/tags/v2.1/License.txt

XFS-for-Windows is distributed under the Mozilla Public License 2.0, which
satisfies the Free Software Definition / Open Source Definition condition of
the WinFsp FLOSS exception. The product links against the platform-specific
WinFsp DLL supplied by the official WinFsp installation and redistributes the
official installer unmodified. No proprietary (non-FLOSS) software is linked
or distributed with WinFsp under this exception.

The WinFsp installer is a shared dependency. It is installed from the official
unmodified MSI and is retained (never removed or altered) when XFS-for-Windows
is upgraded or uninstalled, including when this product's installer installed
it.

Required attribution:

> WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
