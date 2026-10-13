// The page's conformance vectors, for the sealed return and for the request of
// version 2 (contract section 9; the web manager's tests/fixtures/app-auth-vector.json
// holds the same values and the whole address). Both repositories commit them, so a
// change to the page's sealing that the app cannot open, or to the request that the
// page cannot read, fails a test on either side.
//
// Every key and the nonce are FIXED AND NOT SECRET: they protect nothing and are
// to be used for nothing else. The recipe:
//   app secret key        the bytes 1 to 32
//   one-time secret key   the bytes 101 to 132 (the page's, which sealed it)
//   nonce                 24 bytes, 200 down to 177
//   plaintext             {"c":"<683 'A' then '='>","s":"<43 'S'>"}
// and epk, n and d are base58 of the one-time public key, the nonce and the box.
// Opening needs the sdk's crypto (tools/approval-interop-tests.cpp does it); the
// parser's spec only reads the address (tools/auth-link-tests.cpp), and the request
// is built and read back by tools/ur-io-approval-tests.cpp.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace approval_vector {

// 43 'S': the state of the vector's attempt.
inline constexpr std::string_view kState = "SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS";

// The request of version 2, after the page URL and the '#': built from kState, the public key of the app secret key
// above (base58, 43 characters), the second 1790000000 and the API api.beta-test.net. 153 characters, the keys in this
// order.
inline constexpr std::string_view kPublicKey = "WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf";
inline constexpr std::int64_t kStartedUnix = 1790000000;
inline constexpr std::string_view kApi = "api.beta-test.net";
inline constexpr std::string_view kRequestV2 =
    "v=2&platform=windows&state=SSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSSS"
    "&pk=WpSggvhoqoVW4XJvfjsB52dvVunYSQWrz6XiuMQVsCf&ts=1790000000&api=api.beta-test.net";

// base58 of the page's one-time public key (44 characters), of the 24-byte nonce (33)
// and of the sealed box around a 684-character code (1,036).
inline constexpr std::string_view kEpk = "6rvZvM15TRkYozYPXwXPiAMFpXPmiVoTPtCT6uqSLd1X";
inline constexpr std::string_view kNonce = "KJdU5VqRKsTBGyExRrvbGBDpxzurYZSKA";
inline constexpr std::string_view kData =
    "2aX9m44NVjkD3ktAGiNRvA73eNVcqYshgyYoNwdVELq4hfwHuoXff76bA7do6jSEWcRneSFLQUA2XxggBqugr6Ag9bkwBMKhxkUX"
    "keTfv9icHnuL1TdATXcEUDb117puz3KpgWdA54pgqycYfXH5ydtcVdNpstDyCnSpKHKgSG26cmsFKEpAHB7SyEWnRtyEjQTz5pxN"
    "wsexW6ovUHMNmkA8hJEPxjTPJSsSwULKn89au4bwjT8T2NPwRMpcfwfhxfL4KeNz65XN6ee6oDHZmkvnK1h87yeXtURtS8bCrHEq"
    "EFgu4mCxrGchPRaYa4nRKt8ATw3cem5BciWjsb69aGDueQUgNkktTRkA1eTzksqRDuTWofS6D1L1FS6knm3Q1kxXaLjv6NeQqTTi"
    "APiMVVpZbvwwZxh8S6GbTXVnKe7e5aszKP6Vp4EzgRpnxH8r2fTexQDurtztErNs7aoxxNJpb2oWt7mbHAj7nJz927oG5yMk8c4x"
    "q7E32nVvB4sseAhR5Zvg4rAPefkoDGBXNkiKKrvSA6DLjtrqrJ5kmqauBd74RKNsUiGTDHmRMkLSz1GWesgTzmCAqb8pxHzdZwV9"
    "DxEuWzCY58oz8rQpLdLCfySYmwtoLCM1vQFfqgWp2nwbtZ6hTaGKCDzeHfEte7AxZr7VB6H4JRN2e9ow2BK6FerRKxmYipM5Qa4S"
    "k9hNHMNZy49dT74AEcDSeoRFc3u4mncwZ7nRLXkvYJKrkwozPbzXCGLPX8XdUWd6pE8Qcpi78aP4c1KozVpd4ThmAeeW49YixkTp"
    "mq5kL2zBxTQVDjyVbBztxEDq4JGgj2CnXJQ3jEyA9BwkRCHdnSuEPKdfPaYX8X1zTBybVPuN8HVA7oVKXcLmD7EnoAdpVYjfge5M"
    "KbDrNTpKGxNkV7jEqtrBmak9JJkdLWy11iWxn2xYk5i2ibvesicee8ja9SKXP1GKibCJZ52iDgARCsAEd9LqcSUKJH1tQWeTDJzp"
    "n7Zywt3jSRG1YKznWtJAwwVf1x4PosLoT2Co";

}  // namespace approval_vector
