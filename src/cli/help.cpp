#include "vb/cli/help.hpp"

namespace vb::cli {

const std::vector<CommandDoc> &command_docs() {
	static const std::vector<CommandDoc> docs = {
		{ "install",
				"Downloads a release, verifies its checksum (and signature when a trusted key is configured) and unpacks it under the data directory. The first installed version becomes the default. With no version, installs the latest release.",
				{ { "--force", "reinstall even if the version is already installed" },
						{ "--build debug", "install the debug build instead of the release build" },
						{ "--json", "print {version, already_installed, default_set} instead of progress output" } },
				{ { "vb install", "install the latest release" }, { "vb install v0.6.0 --json", "install one version, machine-readable" } },
				{},
				true },
		{ "update", "Installs the latest release next to the existing ones; `vb use` switches the default.",
				{ { "--json", "same as `vb install --json`" } }, { { "vb update", "get the newest release" } }, {}, true },
		{ "prune", "Removes old, non-default releases and leftover staging directories. Versions that a server instance pins or runs from are kept.",
				{ { "--keep N", "keep the newest N releases (default 2)" } }, { { "vb prune --keep 1", "keep only the newest release" } }, {}, false },
		{ "doctor", "Checks the platform, data directory, release source, installed versions and links, and the default version. Exit code 1 when a check fails.",
				{ { "--json", "print {ok, problems, checks:[{level,text}]}" } }, { { "vb doctor --json", "machine-readable health check" } }, {}, true },
		{ "list", "Lists installed versions and links; `*` marks the default.",
				{ { "--remote", "list the versions available to install instead" }, { "--json", "print an array of {name, kind, default, path, exists}" } },
				{ { "vb list --json", "installed versions as JSON" } }, {}, true },
		{ "use", "Sets the default version used when a command is not given `--version`.", {}, { { "vb use v0.6.0", "make v0.6.0 the default" } }, {}, false },
		{ "which", "Prints the path of the client, server or editor binary of a version.",
				{ { "--version <v>", "which installed version (default: the default)" }, { "--json", "print {version, kind, path}" } },
				{ { "vb which server --json", "where is the default server binary" } }, {}, true },
		{ "uninstall", "Removes an installed release (or just the link file for a link). Refused while a server instance depends on the version unless `--force`.",
				{ { "--force", "remove it even if instances depend on it" } }, { { "vb uninstall v0.5.0", "remove one version" } }, {}, false },
		{ "link", "Registers a local build directory as a pseudo-version so engine developers can run `vb host --version <name>` against it.",
				{}, { { "vb link dev ./build", "use ./build as version \"dev\"" } }, {}, false },
		{ "unlink", "Removes a link (never touches the linked build directory).", { { "--force", "remove even if instances use it" } }, { { "vb unlink dev", "forget the link named dev" } }, {}, false },
		{ "launch", "Starts the client of a version in the foreground with the user's shared settings and a per-user singleplayer world directory. Everything after `--` goes to the client.",
				{ { "--version <v>", "which installed version" }, { "--connect host[:port]", "join a server instead of the main menu" } },
				{ { "vb launch --connect localhost:7777", "join a local server" }, { "vb launch -- --loopback", "start the in-process loopback diagnostic" } }, {}, false },
		{ "host", "Runs a dedicated server in the foreground (Ctrl+C stops it). With `--watch` it restarts whenever the pack changes on disk. Everything after `--` goes to the server.",
				{ { "--version <v>", "which installed version" }, { "--port <n>", "UDP port" }, { "--pack <dir>", "content pack directory (default: the bundled base pack); each pack keeps its own world" },
						{ "--watch", "restart the server when files in the pack change" } },
				{ { "vb host --pack ./my_pack --watch", "develop a pack with auto-restart" } }, {}, false },
		{ "server", "Named, persistent server instances (each with its own server.toml, world, logs and pinned version) that run in the background.",
				{ { "--json", "on list/status: machine-readable output" }, { "--yes", "on rm: also delete the saved world without refusing" } },
				{ { "vb server new survival --pack ./my_pack", "create an instance" }, { "vb server start survival", "start it detached" }, { "vb server status --json", "all instances" } },
				{ { "server new <name> [--version v] [--pack dir] [--port n]", "create an instance" },
						{ "server list [--json]", "list instances" },
						{ "server start <name>", "start in the background" },
						{ "server stop <name>", "stop gracefully (saves the world)" },
						{ "server restart <name>", "stop then start" },
						{ "server status [<name>] [--json]", "uptime, players, port" },
						{ "server logs <name> [-f] [-n N]", "show (and follow) the log" },
						{ "server config <name> <get|set|unset|edit> ...", "read or change server.toml" },
						{ "server service <name> <install|remove>", "run as an OS service" },
						{ "server rm <name> [--keep-world] [--yes]", "delete an instance" } },
				true },
		{ "structure", "Author decorative structures (trees, ruins) for a pack: scaffold a data file, edit it visually in the structure editor, validate it headless.",
				{ { "--json", "validate: machine-readable report" } }, { { "vb structure new oak_tree", "scaffold a structure file" } },
				{ { "structure new <name>", "scaffold structures/<name>.lua" }, { "structure edit <file>", "open the structure editor" },
						{ "structure validate [<pack>] [--json]", "check every structure of a pack" } },
				true },
		{ "pack",
				"Create, validate and run a content pack. All subcommands are non-interactive; a pack is a directory with pack.toml, init.lua and optional blocks/, entities/, biomes/, ui/ and textures/.\n\n`vb pack check` and `vb pack dev` choose the newest installed version that satisfies the pack's engine_version_req (or `--version`, which must satisfy it too); if none does they print the `vb install` to run.\n\nFiles in ui/ run in the client UI VM (`ui`, `client`); all others in the server VM (`vb`). `vb pack check` enforces that.",
				{ { "--json", "init/check/types/info: machine-readable output" }, { "--force", "init: write into a non-empty directory; types: overwrite .luarc.json and ui/.luarc.json" },
						{ "--template <t>", "init: minimal (default), ui, worldgen, or base (a copy of the installed content/base)" },
						{ "--name <id>", "init: pack id, [a-z][a-z0-9_]{0,31} (default: the directory name)" },
						{ "--engine-req <range>", "init: engine_version_req to write (default \">=<selected version>\")" },
						{ "--strict", "check: warnings fail the check too" }, { "--version <v>", "which installed version to use" },
						{ "--port <n>", "dev: server port (default 7777)" }, { "--no-client", "dev: only host, do not open the client" } },
				{ { "vb pack init my_pack && cd my_pack", "scaffold a pack" }, { "vb pack check --json", "validate; exit 0 clean, 1 errors" },
						{ "vb pack dev", "host with auto-restart and open the client" }, { "vb pack init --template base my_mod", "start from a copy of base" } },
				{ { "pack init [dir] [--template minimal|ui|worldgen|base] [--name id] [--engine-req range] [--force] [--json]", "scaffold a pack (pack.toml, init.lua, README.md, AGENTS.md, .luarc.json, editor stubs)" },
						{ "pack check [dir] [--json] [--strict] [--version v]", "load the pack headless and print file:line diagnostics" },
						{ "pack dev [dir] [--version v] [--port n] [--no-client]", "host with restart on save and open the client" },
						{ "pack types [dir] [--version v] [--force] [--json]", "(re)write .vb/lua stubs, .luarc.json and ui/.luarc.json" },
						{ "pack info [dir] [--json]", "name, version, engine requirement, file counts" } },
				true },
		{ "docs", "Prints the documentation that ships with the installed version, so an offline agent reads exactly the docs of the engine it targets. No topic lists them; a topic is a file name (`cli.md`, `lua-api.md`) or a Lua name such as `vb.world.raycast`.",
				{ { "--path", "print the docs directory" }, { "--version <v>", "which installed version" } },
				{ { "vb docs vb.world.raycast", "the reference entry for one function" }, { "vb docs --path", "where the docs are" } }, {}, false },
		{ "self", "Updates `vb` itself to the latest release.", { { "--check", "only report whether an update exists" }, { "--force", "update even if up to date" } },
				{ { "vb self update --check", "is there a newer vb" } }, { { "self update [--check] [--force]", "update vb itself" } }, false },
		{ "shim", "Puts `voxel_browser` and `voxel_browser_server` launchers in <data>/bin that run the default version.", {}, { { "vb shim install", "create the launchers" } }, { { "shim install", "create the launchers" }, { "shim remove", "delete them" } }, false },
		{ "completions", "Prints a shell completion script generated from the same command table.", {}, { { "vb completions bash > ~/.vb-completions", "bash" } }, {}, false },
		{ "paths", "Prints the data, config and cache directories (`VB_HOME` relocates all three).", { { "--json", "print {data, config, cache}" } }, { { "vb paths --json", "" } }, {}, true },
	};
	return docs;
}

const CommandDoc *find_command_doc(const std::string &name) {
	for (const CommandDoc &d : command_docs()) {
		if (d.name == name) {
			return &d;
		}
	}
	return nullptr;
}

} // namespace vb::cli
