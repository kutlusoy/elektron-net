// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <common/license_info.h>

#include <tinyformat.h>
#include <util/translation.h>

#include <string>

std::string CopyrightHolders(const std::string& strPrefix)
{
    const auto copyright_devs = strprintf(_(COPYRIGHT_HOLDERS), COPYRIGHT_HOLDERS_SUBSTITUTION).translated;
    std::string strCopyrightHolders = strPrefix + copyright_devs;

    // Make sure Bitcoin Core copyright is not removed by accident
    if (copyright_devs.find("Bitcoin Core") == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The Bitcoin Core developers";
    }
    // Add Elektron Net copyright
    if (copyright_devs.find("Elektron Net") == std::string::npos) {
        strCopyrightHolders += "\n" + strPrefix + "The Elektron Net developers";
    }
    return strCopyrightHolders;
}

std::string LicenseInfo()
{
    const std::string URL_SOURCE_CODE = "<https://github.com/kutlusoy/elektron-net>";

    return CopyrightHolders(strprintf(_("Copyright (C) %i-%i"), 2009, COPYRIGHT_YEAR).translated + " ") + "\n" +
           "\n" +
           strprintf(_("Please contribute if you find %s useful. "
                       "Visit %s for further information about the software."),
                     CLIENT_NAME, "<" CLIENT_URL ">")
               .translated +
           "\n" +
           strprintf(_("The source code is available from %s."), URL_SOURCE_CODE).translated +
           "\n" +
           "\n" +
           _("This is experimental software.") + "\n" +
           strprintf(_("Distributed under the MIT software license, see the accompanying file %s or %s"), "COPYING", "<https://opensource.org/license/MIT>").translated +
           "\n" +
           "\n" +
           _("Express Declaration of Will: Right to Be Forgotten") + "\n" +
           _("By choosing Elektron Net, users express their informed will to exercise their right to be forgotten.") + "\n" +
           _("This project turns a principle recognized in many legal systems into a technical protocol feature: mandatory 137-day pruning automatically and irreversibly removes transaction data from the node after exactly 137 days.") + "\n" +
           _("The source code and reference implementation guarantee this property. Every user who runs Elektron Net supports this philosophy of structural forgetting.") + "\n" +
           _("Important Note:") + "\n" +
           _("Modifying the client to disable or circumvent mandatory pruning while continuing to operate on the same Elektron Net chain is considered a serious violation of the project's core principles and the declared will of its users. Such modifications undermine the right to be forgotten that this chain was built to enforce.") + "\n" +
           _("Creating a completely separate fork or new project is always permitted under the MIT license. However, deliberately defeating the pruning mechanism on the original Elektron Net chain goes against the express intent of this software and its community.") + "\n" +
           strprintf(_("See: %s"), "<https://github.com/kutlusoy/elektron-net/blob/main/right-to-be-forgotten.md>").translated +
           "\n";
}
