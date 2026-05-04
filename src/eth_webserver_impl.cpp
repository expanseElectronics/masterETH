// SPDX-License-Identifier: GPL-3.0-or-later
//
// EthernetWebServer implementation instantiation.
//
// The EthernetWebServer library is structured so that EthernetWebServer.h
// includes EthernetWebServer-impl.h and Parsing-impl.h, which contain the
// full method bodies. Including the .h header from multiple translation
// units therefore produces multiple definitions at link time.
//
// This file is the single translation unit that pulls in the full header,
// causing the library implementation to be emitted exactly once. Every
// other source file in this project includes EthernetWebServer.hpp (via
// dualeth.h), which provides only the declarations.
//
// Pull manager.h first for the central include order (Arduino.h, SPI.h,
// EthernetLarge, EthernetWebServer.hpp). manager.h includes the .hpp
// (declarations only), so pulling the full .h below is what triggers the
// one-and-only instantiation of the EthernetWebServer method bodies.
#include "manager.h"

#include <EthernetWebServer.h>
