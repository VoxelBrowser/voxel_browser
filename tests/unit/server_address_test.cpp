#include <doctest/doctest.h>

#include "vb/core/server_address.hpp"

using vb::core::format_server_address;
using vb::core::parse_server_address;

TEST_CASE("server address: host, host:port and the default port") {
	auto a = parse_server_address("play.example.com");
	REQUIRE(a);
	CHECK(a->host == "play.example.com");
	CHECK(a->port == 27015);

	a = parse_server_address("  10.0.0.5:4000 ");
	REQUIRE(a);
	CHECK(a->host == "10.0.0.5");
	CHECK(a->port == 4000);
}

TEST_CASE("server address: bracketed IPv6") {
	auto a = parse_server_address("[::1]");
	REQUIRE(a);
	CHECK(a->host == "::1");
	CHECK(a->port == 27015);

	a = parse_server_address("[fe80::1]:65535");
	REQUIRE(a);
	CHECK(a->host == "fe80::1");
	CHECK(a->port == 65535);
}

TEST_CASE("server address: rejects bad input with a reason") {
	std::string why;
	for (const char *bad : { "", "   ", ":27015", "host:0", "host:65536", "host:abc", "host:", "host:-1",
				 "::1", "[::1", "[::1]x", "[::1]:0", "a b" }) {
		why.clear();
		CHECK_FALSE_MESSAGE(parse_server_address(bad, &why), bad);
		CHECK_MESSAGE(!why.empty(), bad);
	}
	CHECK(why.size() > 0);
}

TEST_CASE("server address: format round-trips") {
	CHECK(format_server_address("example.com", 27015) == "example.com");
	CHECK(format_server_address("example.com", 1234) == "example.com:1234");
	CHECK(format_server_address("::1", 27015) == "[::1]");
	CHECK(format_server_address("::1", 9) == "[::1]:9");
	auto a = parse_server_address(format_server_address("fe80::2", 99));
	REQUIRE(a);
	CHECK(a->host == "fe80::2");
	CHECK(a->port == 99);
}
