#ifndef HYREMOTE_REMOTEACCESS_EXPORT_H
#define HYREMOTE_REMOTEACCESS_EXPORT_H

// POC_ONLY generated-file shim.
//
// HyRemote's own build generates this header (CMake `generate_export_header`) to decorate the shared
// RemoteAccess library's symbols. The POC fixture compiles HyRemote's RFB transport into a private
// executable instead of linking the DLL, so no export/import annotation applies and the macro is
// deliberately empty.
//
// This file is generated *for* the fixture under the POC build tree. It does not modify anything in
// the HyRemote source tree.

#define HYREMOTE_REMOTEACCESS_EXPORT

#endif  // HYREMOTE_REMOTEACCESS_EXPORT_H
