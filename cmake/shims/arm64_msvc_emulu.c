/* GameNetworkingSockets' bundled ed25519/curve25519-donna uses MSVC's
 * __emulu() (32x32 -> 64 unsigned multiply) when it isn't built for x86-64 or
 * a GNU compiler. MSVC for ARM64 declares it but has no implementation, so the
 * link fails with an unresolved external. Provide it here; see
 * cmake/Dependencies.cmake. Deliberately includes no MSVC intrinsic headers. */
unsigned __int64 __emulu(unsigned int a, unsigned int b) {
	return (unsigned __int64)a * (unsigned __int64)b;
}
