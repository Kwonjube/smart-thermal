// PrinterLink.h — picks the transport this build talks to the printer over.
//
// The only file that knows both link types exist. Everything above the
// transport already speaks to the Transport interface, so changing the
// link is one line in Config.h and nothing else.

#pragma once
#include "Config.h"

#if PRINTER_LINK_W5500
  #include "W5500Transport.h"
  typedef W5500Transport PrinterLink;
#else
  #include "TcpTransport.h"
  typedef TcpTransport PrinterLink;
#endif
