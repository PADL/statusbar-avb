#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// AVTP module header including all format_to() overloads.
///
/// Include this header instead of avtp.hpp when the translation unit
/// needs to call format_to on AVTP types. Headers that only need the
/// data structures should include avtp.hpp (or the specific sub-header)
/// to avoid the compile-time cost of <format>.

#include "statusbar/avtp/avtp.hpp"
#include "statusbar/avtp/avtp_aaf_format.hpp"
#include "statusbar/avtp/avtp_aaf_v1_format.hpp"
#include "statusbar/avtp/avtp_acf_dispatch_format.hpp"
#include "statusbar/avtp/avtp_aef_format.hpp"
#include "statusbar/avtp/avtp_am824_format.hpp"
#include "statusbar/avtp/avtp_am824_v1_format.hpp"
#include "statusbar/avtp/avtp_crf_format.hpp"
#include "statusbar/avtp/avtp_crf_v1_format.hpp"
#include "statusbar/avtp/avtp_dispatch_format.hpp"
#include "statusbar/avtp/avtp_eecf_format.hpp"
#include "statusbar/avtp/avtp_escf_format.hpp"
#include "statusbar/avtp/avtp_ip_encap_format.hpp"
#include "statusbar/avtp/avtp_maap_format.hpp"
#include "statusbar/avtp/avtp_ntscf_format.hpp"
#include "statusbar/avtp/avtp_ntscf_v1_format.hpp"
#include "statusbar/avtp/avtp_tscf_format.hpp"
#include "statusbar/avtp/avtp_tscf_v1_format.hpp"
