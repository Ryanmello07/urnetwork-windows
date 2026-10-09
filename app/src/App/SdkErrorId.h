// The id the sdk's error-id functions answer when the call could not run at
// all: a handle that did not resolve, json that did not decode, a recovered
// panic (URNET_ERROR_ID_INTERNAL in urnetwork_sdk.h, urnet::ErrorIdInternal in
// urnetwork_sdk.hpp). It says nothing about what the user typed, so every
// sheet that maps an sdk error id to its message reads it as
// something_went_wrong, like any id the build does not know
// (VlessPresentation.h ErrorKey, ExtenderPresentation.h ControlDohErrorKey).
//
// The app's one copy of it. The presentation headers that name it stay
// SDK-free, so their harnesses build on any host without the git-ignored
// header, which is why this mirrors the C header's define instead of
// including it. SdkHost.cpp includes the C header for the calls that answer
// the id and holds the two equal at compile time, and
// tests/sdk_error_id_test.go fails on a second copy anywhere in the app or its
// harnesses.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw {

inline constexpr const char* kSdkErrorIdInternal = "internal_error";

}  // namespace urnw
