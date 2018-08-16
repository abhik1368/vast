/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#include <sstream>

#include <caf/ip_address.hpp>

#include "vast/concept/parseable/caf/ip_address.hpp"
#include "vast/concept/parseable/core.hpp"
#include "vast/concept/parseable/numeric.hpp"
#include "vast/concept/parseable/stream.hpp"
#include "vast/concept/parseable/string.hpp"
#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/offset.hpp"
#include "vast/concept/parseable/vast/time.hpp"

#define SUITE parseable
#include "test.hpp"

using namespace vast;
using namespace std::string_literals;

// -- core --------------------------------------------------------------------

TEST(choice) {
  using namespace parsers;
  auto p = chr{'x'} | i32;
  caf::variant<char, int32_t> x;
  CHECK(p("123", x));
  auto i = caf::get_if<int32_t>(&x);
  REQUIRE(i);
  CHECK_EQUAL(*i, 123);
  CHECK(p("x", x));
  auto c = caf::get_if<char>(&x);
  REQUIRE(c);
  CHECK_EQUAL(*c, 'x');
}

TEST(choice triple) {
  using namespace parsers;
  auto fired = false;
  auto p = chr{'x'}
         | i32
         | eps ->* [&] { fired = true; };
  caf::variant<char, int32_t> x;
  CHECK(p("foobar", x));
  CHECK(fired);
}

TEST(list) {
  auto p = parsers::alnum % '.';
  std::vector<char> xs;
  std::string str;
  CHECK(p("a.b.c", xs));
  CHECK(p("a.b.c", str));
  CHECK_EQUAL(xs, (std::vector<char>{'a', 'b', 'c'}));
  CHECK_EQUAL(str, "abc");
}

TEST(maybe) {
  using namespace parsers;
  auto maybe_x = ~chr{'x'};
  auto c = 'x';
  auto f = &c;
  auto l = &c + 1;
  char result = 0;
  CHECK(maybe_x(f, l, result));
  CHECK(f == l);
  CHECK(result == 'x');
  c = 'y';
  f = &c;
  result = '\0';
  CHECK(maybe_x(f, l, result));
  CHECK(f == &c); // Iterator not advanced.
  CHECK(result == '\0'); // Result not modified.
}

TEST(container attribute folding) {
  using namespace parsers;
  auto spaces = *' '_p;
  static_assert(std::is_same_v<decltype(spaces)::attribute, unused_type>,
                "container attribute folding failed");
}

TEST(action) {
  using namespace parsers;
  auto make_v4 = [](uint32_t a) {
    caf::ipv4_address res;
    res.bits(a);
    return caf::ip_address{res};
  };
  auto ipv4_addr = b32be ->* make_v4;
  caf::ip_address x;
  CHECK(ipv4_addr("\x0A\x00\x00\x01", x));
  CHECK_EQUAL(x, *to<caf::ip_address>("10.0.0.1"));
}

// -- string ------------------------------------------------------------------

TEST(char) {
  using namespace parsers;
  MESSAGE("equality");
  auto character = '.';
  auto f = &character;
  auto l = f + 1;
  char c;
  CHECK(chr{'.'}(f, l, c));
  CHECK(c == character);
  CHECK(f == l);

  MESSAGE("inequality");
  character = 'x';
  f = &character;
  CHECK(!chr{'y'}(f, l, c));
  CHECK(f != l);
}

TEST(char class) {
  using namespace parsers;
  MESSAGE("xdigit");
  auto str = "deadbeef"s;
  auto attr = ""s;
  auto f = str.begin();
  auto l = str.end();
  auto p = +xdigit;
  CHECK(p(f, l, attr));
  CHECK(attr == str);
  CHECK(f == l);

  MESSAGE("xdigit fail");
  str = "deadXbeef"s;
  attr.clear();
  f = str.begin();
  l = str.end();
  CHECK(p(f, l, attr));
  CHECK(attr == "dead");
  CHECK(f == str.begin() + 4);
  CHECK(!p(f, l, attr));
  ++f;
  CHECK(p(f, l, attr));
  CHECK(f == l);
  CHECK(attr == "deadbeef");
}

TEST(quoted string) {
  auto p = quoted_string_parser<'\'', '#'>{};
  auto attr = ""s;

  MESSAGE("no escaped chars");
  auto str = "'foobar'"s;
  auto f = str.begin();
  auto l = str.end();
  CHECK(p(f, l, attr));
  CHECK(attr == "foobar");
  CHECK(f == l);

  MESSAGE("escaped char in middle");
  str = "'foo#'bar'"s;
  f = str.begin();
  l = str.end();
  attr.clear();
  CHECK(p(f, l, attr));
  CHECK(attr == "foo'bar");
  CHECK(f == l);

  MESSAGE("escaped char at beginning");
  str = "'#'foobar'"s;
  f = str.begin();
  l = str.end();
  attr.clear();
  CHECK(p(f, l, attr));
  CHECK(attr == "'foobar");
  CHECK(f == l);

  MESSAGE("escaped char at end");
  str = "'foobar#''"s;
  f = str.begin();
  l = str.end();
  attr.clear();
  CHECK(p(f, l, attr));
  CHECK(attr == "foobar'");
  CHECK(f == l);

  MESSAGE("missing trailing quote");
  str = "'foobar"s;
  f = str.begin();
  l = str.end();
  attr.clear();
  CHECK(!p(f, l, attr));
  CHECK(attr == "foobar");

  MESSAGE("missing trailing quote after escaped quote");
  str = "'foobar#'"s;
  f = str.begin();
  l = str.end();
  attr.clear();
  CHECK(!p(f, l, attr));
  CHECK(attr == "foobar'");
}

TEST(symbol table) {
  symbol_table<int> sym{{"foo", 42}, {"bar", 84}, {"foobar", 1337}};
  int i;
  CHECK(sym("foo", i));
  CHECK(i == 42);
  CHECK(sym("bar", i));
  CHECK(i == 84);
  CHECK(sym("foobar", i));
  CHECK(i == 1337);
  i = 0;
  CHECK(!sym("baz", i));
  CHECK(i == 0);
}

TEST(attribute compatibility with string) {
  auto str = "..."s;
  auto attr = ""s;
  auto f = str.begin();
  auto l = str.end();
  auto p = char_parser{'.'};

  MESSAGE("char into string");
  CHECK(p(f, l, attr));
  CHECK(attr == ".");
  CHECK(p(f, l, attr));
  CHECK(attr == "..");
  CHECK(p(f, l, attr));
  CHECK(attr == str);
  CHECK(f == l);

  MESSAGE("plus(+)");
  attr.clear();
  f = str.begin();
  auto plus = +p;
  CHECK(plus(f, l, attr));
  CHECK(str == attr);
  CHECK(f == l);

  MESSAGE("kleene (*)");
  attr.clear();
  f = str.begin();
  auto kleene = *p;
  CHECK(kleene(f, l, attr));
  CHECK(str == attr);
  CHECK(f == l);

  MESSAGE("sequence (>>)");
  attr.clear();
  f = str.begin();
  auto seq = p >> p >> p;
  CHECK(seq(f, l, attr));
  CHECK(str == attr);
  CHECK(f == l);
}

TEST(attribute compatibility with pair) {
  using namespace parsers;
  auto str = "xy"s;
  auto attr = ""s;
  auto f = str.begin();
  auto l = str.end();
  auto c = chr{'x'} >> chr{'y'};

  MESSAGE("pair<char, char>");
  std::pair<char, char> p0;
  CHECK(c(f, l, p0));
  CHECK(p0.first == 'x');
  CHECK(p0.second == 'y');

  MESSAGE("pair<string, string>");
  f = str.begin();
  std::pair<std::string, std::string> p1;
  CHECK(c(f, l, p1));
  CHECK(p1.first == "x");
  CHECK(p1.second == "y");
}

TEST(attribute compatibility with map) {
  using namespace parsers;
  auto str = "a->x,b->y,c->z"s;
  auto f = str.begin();
  auto l = str.end();
  std::map<char, char> map;
  auto p = (any >> "->" >> any) % ',';
  CHECK(p(f, l, map));
  CHECK(f == l);
  CHECK(map['a'] == 'x');
  CHECK(map['b'] == 'y');
  CHECK(map['c'] == 'z');
}

TEST(attribute compatibility with string sequences) {
  using namespace parsers;
  auto p = alpha >> '-' >> alpha >> '-' >> alpha;
  std::string str;
  CHECK(p("x-y-z", str));
  CHECK(str == "xyz");
}

TEST(recursive rule) {
  using namespace parsers;
  rule<std::string::iterator, char> r;
  r = alpha | '[' >> r >> ']';
  auto str = "[[[x]]]"s;
  auto f = str.begin();
  auto l = str.end();

  MESSAGE("unused type");
  CHECK(r(f, l, unused));
  CHECK(f == l);

  MESSAGE("attribute");
  char c;
  f = str.begin();
  CHECK(r(f, l, c));
  CHECK(f == l);
  CHECK(c == 'x');
}

// -- numeric -----------------------------------------------------------------

TEST(bool) {
  auto p0 = single_char_bool_parser{};
  auto p1 = zero_one_bool_parser{};
  auto p2 = literal_bool_parser{};
  auto str = "T0trueFfalse1"s;
  auto i = str.begin();
  auto l = str.end();
  auto f = i;
  bool b;

  MESSAGE("successful 'T'");
  CHECK(p0(i, l, b));
  CHECK(b);
  CHECK(i == f + 1);
  // Wrong parser
  CHECK(!p0(i, l, b));
  CHECK(i == f + 1);
  // Correct parser
  CHECK(p1(i, l, b));
  CHECK(!b);
  CHECK(i == f + 2);
  CHECK(p2(i, l, b));
  CHECK(b);
  CHECK(i == f + 6);
  // Wrong parser
  CHECK(!p2(i, l, b));
  CHECK(i == f + 6);
  // Correct parser
  CHECK(p0(i, l, b));
  CHECK(!b);
  CHECK(i == f + 7);
  b = true;
  CHECK(p2(i, l, b));
  CHECK(!b);
  CHECK(i == f + 12);
  CHECK(p1(i, l, b));
  CHECK(b);
  CHECK(i == f + 13);
  CHECK(i == l);

  MESSAGE("unused type");
  i = f;
  CHECK(p0(i, l, unused));
  CHECK(p0(str));
}

TEST(integral) {
  MESSAGE("signed integers");
  auto str = "-1024"s;
  auto p0 = integral_parser<int>{};
  int n;
  auto f = str.begin();
  auto l = str.end();
  CHECK(p0(f, l, n));
  CHECK(n == -1024);
  CHECK(f == l);
  f = str.begin() + 1;
  n = 0;
  CHECK(p0(f, l, n));
  CHECK(n == 1024);
  CHECK(f == l);
  str[0] = '+';
  f = str.begin();
  n = 0;
  CHECK(p0(f, l, n));
  CHECK(n == 1024);
  CHECK(f == l);

  MESSAGE("unsigned integers");
  auto p1 = integral_parser<unsigned>{};
  unsigned u;
  f = str.begin() + 1; // no sign
  CHECK(p1(f, l, u));
  CHECK(u == 1024);
  CHECK(f == l);
  f = str.begin() + 1;
  u = 0;
  CHECK(p1(f, l, u));
  CHECK(n == 1024);
  CHECK(f == l);

  MESSAGE("digit constraints");
  auto p2 = integral_parser<int, 4, 2>{};
  n = 0;
  str[0] = '-';
  f = str.begin();
  CHECK(p2(f, l, n));
  CHECK(n == -1024);
  CHECK(f == l);
  // Not enough digits.
  str = "-1";
  f = str.begin();
  l = str.end();
  CHECK(!p2(f, l, n));
  CHECK(f == str.begin());
  // Too many digits.
  str = "-123456";
  f = str.begin();
  l = str.end();
  CHECK(!p2(f, l, unused));
  CHECK(f == str.begin());
}

TEST(real) {
  auto p = make_parser<double>{};
  MESSAGE("integral plus fractional part, negative");
  auto str = "-123.456789"s;
  auto f = str.begin();
  auto l = str.end();
  double d;
  CHECK(p(f, l, d));
  CHECK(d == -123.456789);
  CHECK(f == l);
  MESSAGE("integral plus fractional part, positive");
  d = 0;
  f = str.begin() + 1;
  CHECK(p(f, l, d));
  CHECK(d == 123.456789);
  CHECK(f == l);
  MESSAGE("no integral part, positive");
  d = 0;
  f = str.begin() + 4;
  CHECK(p(f, l, d));
  CHECK(d == 0.456789);
  CHECK(f == l);
  MESSAGE("no integral part, negative");
  str = "-.456789";
  f = str.begin();
  l = str.end();
  CHECK(p(f, l, d));
  CHECK(d == -0.456789);
  CHECK(f == l);
  //  MESSAGE("no fractional part, negative");
  //  d = 0;
  //  f = str.begin();
  //  CHECK(p(f, f + 4, d));
  //  CHECK(d == -123);
  //  CHECK(f == str.begin() + 4);
  //  MESSAGE("no fractional part, positive");
  //  d = 0;
  //  f = str.begin() + 1;
  //  CHECK(p(f, f + 3, d));
  //  CHECK(d == 123);
  //  CHECK(f == str.begin() + 4);
}

TEST(byte) {
  using namespace parsers;
  auto str = "\x01\x02\x03\x04\x05\x06\x07\x08"s;
  MESSAGE("single byte");
  auto f = str.begin();
  auto l = f + 1;
  auto u8 = uint8_t{0};
  CHECK(byte(f, l, u8));
  CHECK(u8 == 0x01u);
  CHECK(f == l);
  MESSAGE("big endian");
  f = str.begin();
  l = f + 2;
  auto u16 = uint16_t{0};
  CHECK(b16be(f, l, u16));
  CHECK(u16 == 0x0102u);
  CHECK(f == l);
  f = str.begin();
  l = f + 4;
  auto u32 = uint32_t{0};
  CHECK(b32be(f, l, u32));
  CHECK(u32 == 0x01020304ul);
  CHECK(f == l);
  f = str.begin();
  l = f + 8;
  auto u64 = uint64_t{0};
  CHECK(b64be(f, l, u64));
  CHECK(u64 == 0x0102030405060708ull);
  CHECK(f == l);
  MESSAGE("little endian");
  f = str.begin();
  l = f + 2;
  CHECK(b16le(f, l, u16));
  CHECK(u16 == 0x0201u);
  CHECK(f == l);
  f = str.begin();
  l = f + 4;
  CHECK(b32le(f, l, u32));
  CHECK(u32 == 0x04030201ul);
  CHECK(f == l);
  f = str.begin();
  l = f + 8;
  CHECK(b64le(f, l, u64));
  CHECK(u64 == 0x0807060504030201ull);
  CHECK(f == l);
  MESSAGE("variable length");
  f = str.begin();
  l = f + 3;
  std::array<uint8_t, 3> a3;
  a3.fill(0);
  CHECK(bytes<3>(f, l, a3));
  CHECK(a3[0] == 0x01);
  CHECK(a3[1] == 0x02);
  CHECK(a3[2] == 0x03);
  f = str.begin();
  l = f + 5;
  std::array<uint8_t, 5> a5;
  a5.fill(0);
  CHECK(bytes<5>(f, l, a5));
  CHECK(a5[0] == 0x01);
  CHECK(a5[1] == 0x02);
  CHECK(a5[2] == 0x03);
  CHECK(a5[3] == 0x04);
  CHECK(a5[4] == 0x05);
  std::array<uint8_t, 8> a8;
  CHECK(bytes<8>(str, a8));
  CHECK(a8[0] == 0x01);
  CHECK(a8[1] == 0x02);
  CHECK(a8[2] == 0x03);
  CHECK(a8[3] == 0x04);
  CHECK(a8[4] == 0x05);
  CHECK(a8[5] == 0x06);
  CHECK(a8[6] == 0x07);
  CHECK(a8[7] == 0x08);
  auto ip = "\xdf\x00\x0d\xb8\x00\x00\x00\x00\x02\x02\xb3\xff\xfe\x1e\x83\x28"s;
  std::array<uint8_t, 16> a16;
  CHECK(bytes<16>(ip, a16));
  CHECK(a16[0] == 0xdf);
  CHECK(a16[1] == 0x00);
  CHECK(a16[2] == 0x0d);
  CHECK(a16[3] == 0xb8);
  CHECK(a16[4] == 0x00);
  CHECK(a16[5] == 0x00);
  CHECK(a16[6] == 0x00);
  CHECK(a16[7] == 0x00);
  CHECK(a16[8] == 0x02);
  CHECK(a16[9] == 0x02);
  CHECK(a16[10] == 0xb3);
  CHECK(a16[11] == 0xff);
  CHECK(a16[12] == 0xfe);
  CHECK(a16[13] == 0x1e);
  CHECK(a16[14] == 0x83);
  CHECK(a16[15] == 0x28);
}

TEST(byte - type promotion regression) {
  using namespace parsers;
  uint16_t x;
  CHECK(b16be("\x00\x8d"s, x));
  CHECK_EQUAL(x, 0x8du);
  CHECK(b16le("\x8d\x00"s, x));
  CHECK_EQUAL(x, 0x8du);
  uint32_t y;
  CHECK(b32be("\x00\x00\x00\x8d"s, y));
  CHECK_EQUAL(y, 0x8dul);
  CHECK(b32le("\x8d\x00\x00\x00"s, y));
  CHECK_EQUAL(y, 0x8dul);
  uint64_t z;
  CHECK(b64be("\x00\x00\x00\x00\x00\x00\x00\x8d"s, z));
  CHECK_EQUAL(z, 0x8dull);
  CHECK(b64le("\x8d\x00\x00\x00\x00\x00\x00\x00"s, z));
  CHECK_EQUAL(z, 0x8dull);
}

TEST(dynamic bytes) {
  using namespace parsers;
  std::string foo;
  auto three = 3;
  CHECK(nbytes<char>(three)("foobar"s, foo));
  CHECK_EQUAL(foo, "foo"s);
  MESSAGE("input too short");
  foo.clear();
  auto two = 2;
  CHECK(nbytes<char>(two)("foobar"s, foo));
  CHECK_EQUAL(foo, "fo"s);
  MESSAGE("input too large");
  foo.clear();
  auto seven = 7;
  CHECK(!nbytes<char>(seven)("foobar"s, foo));
  CHECK_EQUAL(foo, "foobar"s);
}

// -- time --------------------------------------------------------------------

TEST(timestamp - now) {
  timestamp ts;
  CHECK(parsers::timestamp("now", ts));
  CHECK(ts > timestamp::min()); // must be greater than the UNIX epoch
}

TEST(timestamp - YMD) {
  using namespace std::chrono;
  timestamp ts;
  CHECK(parsers::timestamp("2017-08-13", ts));
  auto utc_secs = seconds{1502582400};
  CHECK_EQUAL(ts.time_since_epoch(), utc_secs);
  CHECK(parsers::timestamp("2017-08-13+21:10:42", ts));
  utc_secs = std::chrono::seconds{1502658642};
  CHECK_EQUAL(ts.time_since_epoch(), utc_secs);
}

// -- API ---------------------------------------------------------------------

TEST(stream) {
  std::istringstream ss{"1,2,3"};
  offset xs;
  ss >> xs;
  CHECK(ss.good());
  CHECK_EQUAL(xs, (offset{1, 2, 3}));
}

TEST(to) {
  auto xs = to<offset>("1,2,3");
  REQUIRE(xs);
  CHECK_EQUAL(*xs, (offset{1, 2, 3}));
}
