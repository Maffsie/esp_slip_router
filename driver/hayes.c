#include "driver/hayes.h"

/*
 * This aims to be as standard as possible a wrapper for the Hayes AT command
 * set, originally used in the Hayes SmartModem, but due to its reuse in other
 * modems, became retroactively ratified as a standard in ITU V.25ter, later
 * renamed V.250
 * This implementation/emulator follows the spec as closely as reasonably
 * possible, per the referenced texts linked below.
 *
 * TODO:
 * * ATI0-11
 * * AT&F0
 * * AT+W config commands
 * * Persisting settings
 *
 * Other expected functionality not yet available, or impossible/unreasonable
 * to implement:
 * * Baud rate auto-detection
 *    This is normally accomplished by "training" against the AT precursor
 *    but doesn't appear so easy to accomplish on the ESP8266
 * * Phonebook
 *    This is normally provided by a Hayes-compatible modem in order to make
 *    life for the user easier, but does not seem worthwhile implementing here
 *
 * References used while building this emulator:
 * https://en.wikipedia.org/wiki/Hayes_AT_command_set (background research, command reference)
 * https://support.usr.com/support/756/756-ug/six.html (command reference)
 * https://github.com/86Box/86Box/blob/master/src/network/net_modem.c (basic understanding of how AT command parsing can be done)
 * https://opensource.apple.com/source/X11/X11-0.40/xc/programs/Xserver/hw/xfree86/input/mouse/pnp.c.auto.html (for understanding how PnP checksums work)
 * My friend June's serial outputs from her USRobotics dingus. Thanks <3
 */

sysconfig_p config;
hayes_t modem;

LOCAL void ICACHE_FLASH_ATTR h_init_pnp() {
  // Build the P'n'P Identification string (ATI9)
  modem.id = (pnp_t) {
    .begin = '(', // Static
    .upper_rev = 0x01,
    .lower_rev = 0x24,
    // EISA ID is assigned by a governing body, but I haven't heard of them
    //  which makes me think this is something nobody has touched in decades
    .eisa_id = "ESP",
    .prod_id = "ESRH", // ESP SLIP Router - Hayes
    .serial_no = "00000000", // WiFi MAC address?
    .class_id = "MODEM",
    .device_id = "ESPESRH,ATM1152",
    .user_name = "esp-slip-router Hayes-compatible modem",
    // Mod-8 of characters, as hex, MSD first
    .checksum = "00",
    .end = ')' // Static
  };
}
void ICACHE_FLASH_ATTR h_init(sysconfig_p cfg) {
  // Hayes Initialise - set everything to a known state
  config = cfg;
  modem = (hayes_t){.id={0},.prefs={0},.state={0}};
  h_init_pnp();
  modem.prefs.echo = true; // E1
  modem.prefs.report = 7; // X7
  modem.prefs.verbose = true; //V1
  // Register settings
  REG_ESC = ESC_CHR; // Escape character, default +
  REG_CR = CR; // Carriage return character
  REG_LF = LF; // Linefeed character
  REG_BS = BS; // Backspace character
  REG_XON = DC1; // Software flow control on
  REG_XOFF = DC3; // Software flow control off
  
  // S0: Auto-answer after N rings
  // S1: Count and store rings from inbound calls
  REG_S(6) = 2;   // S6, 
  REG_S(7) = 60;  // S7,  Wait time for carrier signal, seconds.
  REG_S(8) = 2;   // S8:  Pause time for each comma in dialstring, seconds.
  REG_S(9) = 6;   // S9:  Carrier detect time, 1/10th second.
  REG_S(10) = 7;  // S10: Carrier loss wait time, 1/10th second
  REG_S(11) = 70; // S11: Tone duration and interval, milliseconds.
  REG_S(12) = 50; // S12: Escape code guard time, half-seconds.
  // S19: Inactivity/hang-up timer
  REG_S(21) = 10; // S21: Break time, 1/100th second.
  // S24: Pulsed DSR duration, 1/50th second.
  REG_S(25) = 5;  // S25: DTR recognition time, 1/100th second.
  // S26: RTS/CTS delay time, 1/100th second
  // S38: Disconnect wait time, seconds.
  // S41: Allowable remote log-in attempts.
  // S42: Remote access ASCII character
  // S43: Remote guard time, 1/5th second.
  // S44: Leased line delay timer
  
  if(HAYES_CMD_MODE_AT_BOOT)
    modem.state.on_hook = true;
  else {
    modem.state.on_hook = false;
    modem.state.in_call = true;
    modem.state.online = true;
  }
}
LOCAL bool ICACHE_FLASH_ATTR h_is_num(char c) {
  // Basic ASCII is-numeric test
  return c > 47 && c < 59;
}
static uint8_t ICACHE_FLASH_ATTR h_parse_num(char c) {
  // Basic single-char atoi
  //  intended for use after validating with h_is_num(char)
  return c-48;
}
LOCAL uint8_t ICACHE_FLASH_ATTR h_multi_parse_num(uint8_t i) {
  // Multiple-digit parser. Gross, but I'm not confident with strcpy/etc
  //  so this absolutely can and should be improved. An atoi() analogue should
  //  never be this bad.
  uint8_t n = 0;
  if(!h_is_num(modem.state.cmdbuf[i])) return 0;
  if(i+1 < modem.state.cmd_i && h_is_num(modem.state.cmdbuf[i+1])) {
    if(i+2 < modem.state.cmd_i && h_is_num(modem.state.cmdbuf[i+2]))
      n += 100*h_parse_num(modem.state.cmdbuf[i++]);
    n += 10*h_parse_num(modem.state.cmdbuf[i++]);
  }
  n += h_parse_num(modem.state.cmdbuf[i]);
  return n;
}
LOCAL void ICACHE_FLASH_ATTR h_echo(char c) {
  // Character echo
  // Controlled by ATE command
  if(!modem.prefs.echo) return;
  // LF is swallowed if last character was CR
  if(c == REG_LF && modem.state.l_chr == REG_CR) return;
  CHAR_OUT(c);
}
LOCAL void ICACHE_FLASH_ATTR h_print_nocr(char s[]) {
  // Stupid-simple print function.
  uint16_t i=0;
  uint16_t sz=os_strlen(s);
  while(i<sz)
    CHAR_OUT(s[i++]);
}
LOCAL void ICACHE_FLASH_ATTR h_print(char s[]) {
  // Wrapper around h_print_nocr to end it with a CR
  h_print_nocr(s);
  CHAR_OUT(REG_CR);
}
LOCAL void h_print_i(uint8_t n) {
  // Print an integer
  // This feels like a wildly inefficient way of accomplishing this.
  char s[4]; // uint8_t can at most be 255, + NUL
  os_sprintf(s, "%u", n);
  h_print(s);
}
LOCAL void ICACHE_FLASH_ATTR h_print_h(char suffix[]) {
  // Used for ATI commands as a header
  char str[60];
  os_sprintf(str, "%s %s", S_ID, suffix);
  h_print(str);
}
LOCAL void ICACHE_FLASH_ATTR h_serialise_pnp(char *buffer[]) {
  char pnpstr[256]; // PNP spec says this may be up to 256 chars
  // (1.0ESPESRH\01234567\MODEM\PNPC10E,PNPC103,PNPC107,PNPC10F\esp-slip-router Hayes-compatible modem\XX)
  /*
  // The following commented out until I can figure out how this checksum stuff works
  os_sprintf(pnpstr, "%c%u%u%s%s\%s\%s\%s\%s\%c",
  modem.id.begin,
  modem.id.upper_rev, modem.id.lower_rev,
  modem.id.eisa_id, modem.id.prod_id,
  modem.id.serial_no,
  modem.id.class_id,
  modem.id.device_id,
  modem.id.user_name,
  modem.id.end);
  uint8_t rem = uint8_t len = os_strlen(pnpstr);
  uint8_t sum = 0;
  while(--rem>=0)
    sum += pnpstr[(len-1)-rem];
  */
  os_sprintf(buffer, "%c%u%u%s%s%c%c%s%c%s%c",
  PNP_BEGIN, modem.id.upper_rev, modem.id.lower_rev,
  modem.id.eisa_id, modem.id.prod_id, PNP_EXTRA, PNP_EXTRA,
  modem.id.class_id, PNP_EXTRA, modem.id.device_id, PNP_END);
}
LOCAL void ICACHE_FLASH_ATTR h_result_connbaud() {
  // Method to output the correct result upon connecting
  if(modem.prefs.quiet) return; // controlled by ATQ
  if(modem.prefs.verbose) { // controlled by ATV
    // Prints "CONNECT <baudrate>"
    char connstr[15];
    os_sprintf(connstr, RESP_CONBAUD, config->bit_rate);
    h_print(connstr);
    return;
  }
  switch(config->bit_rate) { // if ATV0 and ATQ0, map baudrate to response code
    // TODO: Baud rates above 56000 - USRobotics' docs don't include this.
    case 56000: h_print_i(232); break;
    case 54666: h_print_i(228); break;
    case 53333: h_print_i(224); break;
    case 52000: h_print_i(220); break;
    case 50666: h_print_i(216); break;
    case 49333: h_print_i(212); break;
    case 48000: h_print_i(208); break;
    case 46666: h_print_i(204); break;
    case 45333: h_print_i(200); break;
    case 44000: h_print_i(196); break;
    case 42666: h_print_i(192); break;
    case 41333: h_print_i(188); break;
    case 37333: h_print_i(184); break;
    case 33333: h_print_i(180); break;
    case 33600: h_print_i(155); break;
    case 31200: h_print_i(151); break;
    case 28800: h_print_i(107); break;
    case 26400: h_print_i(103); break;
    case 24000: h_print_i( 99); break;
    case 21600: h_print_i( 91); break;
    case 19200: h_print_i( 85); break;
    case 16800: h_print_i( 43); break;
    case 14400: h_print_i( 25); break;
    case 12000: h_print_i( 21); break;
    case  7200: h_print_i( 20); break;
    case  4800: h_print_i( 18); break;
    case  1200: h_print_i( 15); break;
    case  9600: h_print_i( 13); break;
    case  2400: h_print_i( 10); break;
    // If baud rate can't be mapped to a code, just return CONNECT
    default   : h_print_i(1);
  }
}
LOCAL void ICACHE_FLASH_ATTR h_result_send(char verbose[], uint8_t code) {
  // Method to print either the result code, or the result string,
  //  respecting the EQV settings.
  if(modem.prefs.quiet) return;
  if(modem.prefs.verbose) h_print(verbose);
  else h_print_i(code);
}
void ICACHE_FLASH_ATTR h_result(hayes_result_t res) {
  // Maps a result to a string/numeric result code.
  if(modem.prefs.quiet) return;
  switch(res) {
    case OKAY:
      h_result_send(RESP_OK, 0);
      break;
    case CONNECT:
      h_result_send(RESP_CON, 1);
      break;
    case RING:
      h_result_send(RESP_RING, 2);
      break;
    case NO_CARRIER:
      h_result_send(RESP_NOCAR, 3);
      break;
    case ERROR:
      h_result_send(RESP_ERR, 4);
      break;
    case CONNECT_BAUD:
      h_result_connbaud();
      break;
    case NO_DIAL_TONE:
      h_result_send(RESP_NODT, 6);
      break;
    case LINE_BUSY:
      h_result_send(RESP_BUS, 7);
      break;
    case NO_ANSWER:
      h_result_send(RESP_NOANS, 8);
      break;
    case RINGING:
      h_result_send(RESP_RR, 11);
      break;
    default:
      {
        char output[20];
        os_sprintf(output, "????? %u", res);
        h_print(output);
      }
  }
}
LOCAL void ICACHE_FLASH_ATTR h_dial(bool go_online) {
  // Handle "dialling".
  /*
   * TODO:
   * Check if wifi connection is established
   * Return NO_CARRIER or NO_DIAL_TONE if not
   * But only if the dialled number isn't something documented
   *  (for configuring via telnet)
   */
  modem.state.in_call = true;
  modem.state.online = go_online;
  h_result(CONNECT_BAUD);
}

// AT command implementations
// Commands that take an argument will return a bool indicating if that
//  argument was used, as boolean commands can be shortened to just their
//  letter if the desired effect is to set them to false.
// Commands that will terminate the command parse process are prefixed 'ht_'
LOCAL void ICACHE_FLASH_ATTR ht_ATDOLLAR() {
  // AT$ - List all supported commands
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR ht_ATAMPDOLLAR() {
  // AT&$ - List all available vendor commands
  // TODO.
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR ht_ATEXTDOLLAR() {
  // AT+$ - List all available extended commands
  // TODO.
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR hz_ATA() {
  // ATA - Answer
  modem.state.on_hook=false;
  modem.state.in_call=true;
  h_result(OKAY);
  return;
}
LOCAL bool ICACHE_FLASH_ATTR h_ATA(char a) {
  // ATA0 - Answer 0
  // Possibly the argument here is for if you have more than one line/"call"?
  // Just maps to off-hook, in-call for now.
  if(a != '0') return false;
  modem.state.on_hook=false;
  modem.state.in_call=true;
  h_result(OKAY);
  return true;
}
LOCAL void ICACHE_FLASH_ATTR ht_ATDDOLLAR() {
  // ATD$ - List all available dial commands
  // TODO.
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR ht_ATDL() {
  // ATDL - Redial last dialled number
  modem.state.on_hook = false;
  h_dial(true);
}
LOCAL uint8_t ICACHE_FLASH_ATTR ht_ATDN(uint8_t i) {
  // ATD[PRT] - Dial touch-tone, pulse or originate-only line
  uint8_t taken = 1;
  uint8_t pause_time = 0;
  modem.state.on_hook = false;
  bool go_online = true;
  // TODO: Store "dialled" number somewhere, implement pauses
  while(i+taken<modem.state.cmd_i) {
    if(!h_is_num(modem.state.cmdbuf[i+taken]) &&
      modem.state.cmdbuf[i+taken] != ',' && // 2s pause before resuming dial
      modem.state.cmdbuf[i+taken] != '@' && // Wait for answer (X3, X4)
      modem.state.cmdbuf[i+taken] != '.' && // Not in spec, allows dialling IPs
      modem.state.cmdbuf[i+taken] != 'W' && // Wait for second dialtone (X2, X4)
      modem.state.cmdbuf[i+taken] != '#' && // Aux tone dial digit
      modem.state.cmdbuf[i+taken] != '!' && // Switch hook flash
      modem.state.cmdbuf[i+taken] != '$' && // Wait for calling-card bong
      modem.state.cmdbuf[i+taken] != '&' && // Wait for calling-card bong
      modem.state.cmdbuf[i+taken] != ';' && // Remain in command mode after dial
      modem.state.cmdbuf[i+taken] != '*' && // Aux tone dial digit
      modem.state.cmdbuf[i+taken] != '"') { // Set quote mode for the following?
      break;
    } else {
      switch(modem.state.cmdbuf[i+taken]) {
        case ';':
          go_online = false;
          break;
        case ',':
        case 'W':
        case '@':
          pause_time += 2;
          break;
      }
    }
    taken++;
  }
  h_dial(go_online);
  return taken;
}
LOCAL void ICACHE_FLASH_ATTR hz_ATD() {
  // ATD - Dial (no arguments)
  h_result(ERROR);
}
LOCAL uint8_t ICACHE_FLASH_ATTR ht_ATD(uint8_t i) {
  // ATD - Dial
  // Takes in the current location in the command buffer + 1
  // Returns how many characters it consumed
  switch(modem.state.cmdbuf[i]) {
    case 'L':
      ht_ATDL();
      return 1;
    case 'P': // Pulse-dial
    case 'R': // Dial an originate-only modem
    case 'T': // Touch-tone dial
      return ht_ATDN(i);
    case 'S': // Dial a stored number
      h_result(ERROR);
      return 1;
    case '$':
      ht_ATDDOLLAR();
      return 1;
  }
  if(h_is_num(modem.state.cmdbuf[i])) return ht_ATDN(--i)-1;
  h_result(ERROR);
  return 40-i;
}
LOCAL bool ICACHE_FLASH_ATTR h_ATE(char a) {
  // ATE[0,1] - Echo on/off
  h_result(OKAY);
  if(a != '0' && a != '1') {
    modem.prefs.echo = false;
    return false;
  }
  modem.prefs.echo = a == '1';
  return true;
}
LOCAL void ICACHE_FLASH_ATTR hz_ATE() {
  // ATE - Echo off
  modem.prefs.echo = false;
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR hz_ATI() {
  // ATI - Should error if missing argument.
  h_result(ERROR);
}
LOCAL void ICACHE_FLASH_ATTR ht_ATI(uint8_t i) {
  // ATI0-11 - Inform, Inquire, Interrogate
  // Outputs various information about the modem, its state, configuration, etc
  if(!h_is_num(modem.state.cmdbuf[i])) {
    h_result(ERROR);
    return;
  }
  uint8_t method = h_multi_parse_num(i);
  char outstr[256];
  uint8_t iter=0;
  switch(method) {
    case 0:
      // ATI0 - Model string
      h_print("ESP_SR");
      break;
    case 1:
      // ATI1 - ROM Checksum (4 characters)
      // TODO: This should be an actual checksum of the ESP's flash.
      h_print("A0B1");
      break;
    case 2:
      // ATI2 - RAM test results
      // Just outputs "OK".
      break;
    case 3:
      // ATI3 - Firmware version
      h_print(ESP_SLIP_ROUTER_VERSION);
      break;
    case 4:
      // ATI4 - Settings
      // TODO.
      /*
       * Lists states for B, C, E, F, L, M, Q, V and X settings
       * Lists baud, parity, length
       * "DIAL=HUNT"? "ON HOOK" "TIMER"
       * Lists states for &A, B, C, D, ...
       * Lists register states
       * Lists last dialed number
       */
      h_print_h("Settings...");
      // Basic settings/state
      os_sprintf(outstr,
      "E%u L2 M1 Q%u V%u X%u%c"
      "BAUD=%u PARITY=N WORDLEN=8%c"
      "DIAL=HUNT O%s HOOK TIMER%c",
      modem.prefs.echo, modem.prefs.quiet, modem.prefs.verbose,
      modem.prefs.report, REG_CR,
      config->bit_rate, REG_CR,
      modem.state.on_hook? "N " : "FF", REG_CR);
      h_print(outstr);
      // Registers
      os_sprintf(outstr,
        "S00=%03u  S01=%03u  S02=%03u  S03=%03u  S04=%03u  S05=%03u  S06=%03u  S07=%03u",
        REG_E(0), REG_E(1), REG_E(2), REG_E(3), REG_E(4), REG_E(5), REG_E(6), REG_E(7));
      os_sprintf(outstr,
        "S00=%03u  S01=%03u  S02=%03u  S03=%03u  S04=%03u  S05=%03u  S06=%03u  S07=%03u",
        REG_E(0), REG_E(1), REG_E(2), REG_E(3), REG_E(4), REG_E(5), REG_E(6), REG_E(7));
      os_sprintf(outstr,
        "S00=%03u  S01=%03u  S02=%03u  S03=%03u  S04=%03u  S05=%03u  S06=%03u  S07=%03u",
        REG_E(0), REG_E(1), REG_E(2), REG_E(3), REG_E(4), REG_E(5), REG_E(6), REG_E(7));
      break;
    case 5:
      // ATI5 - NVRAM Settings
      // TODO after preservation of settings is implemented.
      // lists most of the same as above but also phonebook and extra settings
      // also stored command(?)
      break;
    case 6:
      // ATI6 - Link diagnostics
      // TODO.
      // Chars, Octets, Blocks sent/recv
      // Chars lost
      // Blocks resent
      // Retrains req/granted
      // Line reversals
      // "Blers"
      // Link timeouts/naks
      // Compression, Equalization, fallback, last call length, current state
      break;
    case 7:
      // ATI7 - configuration profile
      // Not sure what this would map to, it should probably just return ERROR
      break;
    // ATI8 - (riker voice) that never happened.
    case 9:
      // ATI9 - Plug'n'Play string
      /*
       * Per MS' External COM Device Specification, 0.99D, 17th Feb., 1995
       * PnP can be implemented as a state machine I think, but may not be
       *  worthwhile to really do properly.
       */
       h_serialise_pnp(&outstr);
       h_print(outstr);
      break;
    case 10:
      // ATI10 - Dial security status
      // long listing on this
      // Could be used to display current lock status for router
      break;
    case 11:
      // ATI11 - More link diagnostics
      // Modulation, carrier freq., sym rate, encoding, shaping
      // Signal/noise levels, echo loss, timing, up/down/speed shifts
      // V.90 status
      break;
    case 19:
      // ATI19 - Deliberately high
      // Outputs the current modem state.
      os_sprintf(outstr, "E%uQ%uV%uX%u",
                 modem.prefs.echo,
                 modem.prefs.quiet,
                 modem.prefs.verbose,
                 modem.prefs.report);
      h_print(outstr);
      h_print("cmdbuf:");
      while(iter<40) {
        os_sprintf(outstr, "%u: %c (%u, %x)    ", iter, modem.state.cmdbuf[iter], modem.state.cmdbuf[iter], modem.state.cmdbuf[iter]);
        h_print_nocr(outstr);
        if(iter++%4==0)
          CHAR_OUT(REG_CR);
      }
      CHAR_OUT(REG_CR);
      os_sprintf(outstr, "cmdbuf index %u, last %u, lchr %c",
                 modem.state.cmd_i, modem.state.l_cmd_i,
                 modem.state.l_chr);
      h_print(outstr);
      os_sprintf(outstr, "online=%u on-hook=%u in-cmd=%u n-escs=%u",
                 modem.state.online, modem.state.on_hook,
                 modem.state.in_cmd, modem.state.n_escs);
      h_print(outstr);
    default:
      h_result(ERROR);
      return;
  }
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR htz_ATH() {
  // ATH - Hangup
  // Whether a call is in progress or not, this always returns 'OKAY'
  //  on real hardware.
  modem.state.on_hook = true;
  modem.state.in_call = false;
  h_result(OKAY);
}
LOCAL bool ICACHE_FLASH_ATTR ht_ATH(char c) {
  if(c != '0' && c != '1') {
    htz_ATH();
    return false;
  }
  modem.state.on_hook = c == '0';
  return true;
}
LOCAL void ICACHE_FLASH_ATTR ht_ATO() {
  // ATO - Enter on-line mode
  // Output validated to be consistent with real hardware.
  if(modem.state.on_hook || !modem.state.in_call) h_result(NO_CARRIER);
  else {
    modem.state.online = true;
    h_result(OKAY);
  }
}
LOCAL bool ICACHE_FLASH_ATTR h_ATQ(char a) {
  // ATQ[0,1] - Quiet on/off
  h_result(OKAY);
  if(a != '0' && a != '1') {
    modem.prefs.quiet = false;
    return false;
  }
  modem.prefs.quiet = a == '1';
  return true;
}
LOCAL void ICACHE_FLASH_ATTR hz_ATQ() {
  // ATQ - Quiet off
  modem.prefs.quiet = false;
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR hz_ATS() {
  // ATS - Set/interrogate/list registers
  // Error on no arguments.
  h_result(ERROR);
}
LOCAL void ICACHE_FLASH_ATTR hz_ATSDOLLAR() {
  h_result(OKAY);
}
LOCAL uint8_t ICACHE_FLASH_ATTR h_ATS(uint8_t i) {
  if(modem.state.cmdbuf[i] == '$') {
    hz_ATSDOLLAR();
    return 1;
  }
  if(!h_is_num(modem.state.cmdbuf[i])) {
    h_result(ERROR);
    return 40-i;
  }
  uint8_t taken = 0;
  uint8_t reg = h_multi_parse_num(i);
  if((reg>13 && reg<16) || (reg>25 && reg<38) || reg>38 || reg==17 || reg==20 || reg==24) {
    h_result(ERROR);
    return 40-i;
  }
  taken += reg>9? 2 : 1;
  // Parse intent
  char buf[20]; // can't declare inside a switch block
  switch(modem.state.cmdbuf[i+taken++]) {
    case '?':
      // ATSn? - Interrogate the register's contents
      if((reg>1 && reg<6) || (reg>21 && reg<24))
        // These registers are chars
        os_sprintf(buf, S_REG_C, reg, modem.prefs.regs[reg]);
      else
        os_sprintf(buf, S_REG_I, reg, modem.prefs.regs[reg]);
      h_print(buf);
      h_result(OKAY);
      break;
    case '=':
      // ATSn=v - Set a register to a value
      if((reg>1 && reg<6) || (reg>21 && reg<24))
        modem.prefs.regs[reg] = modem.state.cmdbuf[taken+i];
      else modem.prefs.regs[reg] = h_multi_parse_num(taken+i);
      h_result(OKAY);
      break;
    default:
      h_result(ERROR);
      return 40-(i+taken); // Prevent further command execution
  }
  return taken;
}
LOCAL bool ICACHE_FLASH_ATTR h_ATV(char a) {
  // ATV[0,1] - Verbose on/off
  h_result(OKAY);
  if(a != '0' && a != '1') {
    modem.prefs.verbose = false;
    return false;
  }
  modem.prefs.verbose = a == '1';
  return true;
}
LOCAL void ICACHE_FLASH_ATTR hz_ATX() {
  h_result(ERROR);
}
LOCAL bool ICACHE_FLASH_ATTR h_ATX(char c) {
  if(!h_is_num(c)) return false;
  if(c-48 > 7) {
    h_result(ERROR);
    return true;
  }
  modem.prefs.report = c-48;
  return true;
}
LOCAL void ICACHE_FLASH_ATTR hz_ATV() {
  // ATV - Verbose off
  modem.prefs.verbose = false;
  h_result(OKAY);
}
LOCAL void ICACHE_FLASH_ATTR ht_ATZ() {
  // ATZ - Restart / Reset
  system_restart();
  while(true);
}

// AT&...
LOCAL void ht_AMP_F(char c) {
  // AT&Fn - Reset to factory settings
  if(c != '0') {
    h_result(ERROR);
    ht_ATZ();
  }
  ht_ATZ();
}

// AT+...
// V.250-compliant commands

LOCAL void ICACHE_FLASH_ATTR h_cmdparse_amp(uint8_t i) {
  uint8_t taken=0;
  switch(modem.state.cmdbuf[i]) {
    case 'F':
      if(i+1 == modem.state.cmd_i) h_result(ERROR);
      else ht_AMP_F(modem.state.cmdbuf[++taken+i]);
      return;
    default:
      return ++taken;
  }
}
LOCAL void ICACHE_FLASH_ATTR h_cmdparse_plus(uint8_t i) {
  uint8_t taken=0;
  switch(modem.state.cmdbuf[i]) {
    default:
      return ++taken;
  }
}
LOCAL void ICACHE_FLASH_ATTR h_cmdparse() {
  if(modem.state.cmd_i==0) {
    h_result(OKAY);
    return;
  }
  uint8_t i=0;
  while(i<modem.state.cmd_i) {
    switch(modem.state.cmdbuf[i]) {
      case 'A': // ATA[n] - Answer
        if(i+1 == modem.state.cmd_i) hz_ATA();
        else if(!h_ATA(modem.state.cmdbuf[++i])) i--;
        break;
      case 'D':
        // ATD[LPRS$][n...] - Dial number
        if(i+1 == modem.state.cmd_i) hz_ATD();
        else i+=ht_ATD(++i);
        return;
      case 'E':
        if(i+1 == modem.state.cmd_i) hz_ATE();
        else if(!h_ATE(modem.state.cmdbuf[++i])) i--;
        break;
      // Not implemented: ATF[n] - Online echo
      case 'H':
        if(i+1 == modem.state.cmd_i) htz_ATH();
        else if(!ht_ATH(modem.state.cmdbuf[++i])) i--;
        break;
      case 'I':
        if(i+1 == modem.state.cmd_i) hz_ATI();
        else ht_ATI(++i);
        break;
      case 'L': // Modem speaker volume
      case 'M': // Modem speaker mode
        if(i+1 != modem.state.cmd_i && h_is_num(modem.state.cmdbuf[++i]))
          h_result(OKAY);
        else h_result(ERROR);
        break;
      case 'O':
        if(i+1 == modem.state.cmd_i) ht_ATO();
        else ht_ATO(++i);
        return;
      case 'Q':
        if(i+1 == modem.state.cmd_i) hz_ATQ();
        else if(!h_ATQ(modem.state.cmdbuf[++i])) i--;
        break;
      case 'S':
        // ATS$, ATSn?, ATSn=v
        if(i+1 == modem.state.cmd_i) h_result(ERROR);
        else i+=h_ATS(++i);
        break;
      case 'V':
        if(i+1 == modem.state.cmd_i) hz_ATV();
        else if(!h_ATV(modem.state.cmdbuf[++i])) i--;
        break;
      case 'X':
        if(i+1 == modem.state.cmd_i) hz_ATX();
        else if(!h_ATX(modem.state.cmdbuf[++i])) i--;
        break;
      case 'Z':
        ht_ATZ();
        return;
      case '&':
        if(i+1 == modem.state.cmd_i) h_result(ERROR);
        else i+=h_cmdparse_amp(++i);
        break;
      case '$':
        if(i>0 && modem.state.cmdbuf[i-1] == '&') ht_ATAMPDOLLAR();
        else ht_ATDOLLAR();
        return;
    }
    i++;
  }
}
LOCAL void ICACHE_FLASH_ATTR h_recv(char c) {
  h_echo(c);
  if(modem.state.in_cmd) {
    if(c == REG_CR) {
      h_cmdparse();
      modem.state.in_cmd = false;
      modem.state.l_cmd_i = modem.state.cmd_i;
      modem.state.cmd_i = 0;
    } else if(c == REG_BS && modem.state.cmd_i>0) {
      modem.state.cmd_i--;
      if(modem.state.cmd_i>0)
        modem.state.l_chr = modem.state.cmdbuf[modem.state.cmd_i];
      return; // Skip setting l_chr
    } else if(modem.state.cmd_i == 40) {
      h_result(ERROR);
      modem.state.in_cmd = false;
      modem.state.l_cmd_i = modem.state.cmd_i = 0;
    } else
      modem.state.cmdbuf[modem.state.cmd_i++] = c;
  } else if(modem.state.l_chr == 'A') {
    switch(c) {
      case '/':
        modem.state.in_cmd = true;
        modem.state.cmd_i = modem.state.l_cmd_i;
        h_cmdparse();
        modem.state.in_cmd = false;
        modem.state.cmd_i = 0;
        break;
      case 'T':
        modem.state.in_cmd = true;
        break;
    }
  }
  // Not implemented: bare '/' (Pause)
  // Pause should wait 125ms before processing further input
  // The docs mention 125ms as a default, but don't indicate if it can change
  modem.state.l_chr = c;
}
// Returns true if input was handled
bool ICACHE_FLASH_ATTR h_handler(char c, void (*slip_rx)(struct netif*, u8_t), struct netif *slip_if, uint64_t *bytes_out) {
  if(!modem.state.online) {
    h_recv(c);
    return true;
  }
  if(c == REG_ESC) {
    if(modem.state.in_esc && modem.state.n_escs == 2) {
      modem.state.in_esc = false;
      modem.state.n_escs = 0;
      modem.state.online = false;
      h_result(OKAY);
      return true;
    }
    if(!modem.state.in_esc)
      modem.state.in_esc = true;
    modem.state.n_escs++;
    return true;
  }
  if(modem.state.in_esc) {
    modem.state.in_esc = false;
    while(modem.state.n_escs-->0) {
      slip_rx(slip_if, REG_ESC);
      (*bytes_out)++;
    }
  }
  return false;
}
