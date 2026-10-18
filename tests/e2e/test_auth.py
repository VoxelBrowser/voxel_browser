"""In-engine authentication end to end (architecture_spec/auth.md, Phase 9.7).

A real server whose pack ships an `auth.lua` pointing at a mock OpenID Connect provider
(vbtest/mock_keycloak.py), and real headless clients that sign in with `--auth-token-file` or, for
the browser flow, a test that plays the user's browser. The mock IdP signs with a TEST-ONLY
key from tests/e2e/fixtures/.
"""
import threading
import time
import urllib.request

import pytest

from vbtest import ProcessDied, expect


def token_file(tmp_path, token, name="token.txt"):
    f = tmp_path / name
    f.write_text(token)
    return f


def test_token_file_sign_in_joins_under_the_verified_name(idp, auth_server, make_clients, tmp_path):
    server = auth_server()
    f = token_file(tmp_path, idp.mint("sub-alice", "alice", claims={"email": "a@example.com"}))
    (alice,) = make_clients(server)(1, names=["ClientChosenName"], extra_args=["--auth-token-file", str(f)])
    # The in-game name is the verified claim, never what the client asked for.
    expect(server).to_have_player_login("sub-alice", name="alice", provider="keycloak")
    expect(server).to_have_player_count(1)


def test_login_reaches_pack_scripts(idp, auth_server, make_clients, tmp_path):
    server = auth_server()
    server.run_lua('''
        vb.on("chat", function(player, text)
            local l = player:get_login()
            player:send_message("login=" .. (l and (l.subject .. "/" .. l.claims.email) or "nil"))
        end)
    ''')
    f = token_file(tmp_path, idp.mint("sub-bob", "bob", claims={"email": "b@example.com", "secret": "x"}))
    (bob,) = make_clients(server)(1, extra_args=["--auth-token-file", str(f)])
    bob.chat("hi")
    expect(bob).to_have_chat("login=sub-bob/b@example.com")


def test_ui_scripts_see_who_is_signed_in(idp, auth_server, make_clients, tmp_path):
    # The UI VM's state.login (the server-given name + the IdP subject); base:pause shows it.
    server = auth_server()
    f = token_file(tmp_path, idp.mint("sub-dana", "dana"))
    (dana,) = make_clients(server)(1, names=["NotDana"], extra_args=["--auth-token-file", str(f)])
    dana.key_press("pause")
    expect(dana).to_have_ui_open("base:pause")
    expect(dana).to_have_widget("signed_in", text="Signed in as dana")


@pytest.mark.parametrize("why,kwargs", [
    ("expired", {"iat": int(time.time()) - 4000, "exp": int(time.time()) - 3000}),
    ("wrong audience", {"aud": "someone-else"}),
    ("wrong issuer", {"iss": "https://evil.example/realms/x"}),
    ("tampered payload", {"tamper": True}),
    ("unknown signing key id", {"kid": "not-in-the-jwks"}),
    ("not RS256/ES256", {"alg": "HS256"}),
])
def test_bad_tokens_never_join(idp, auth_server, make_clients, tmp_path, why, kwargs):
    server = auth_server()
    f = token_file(tmp_path, idp.mint("sub-mallory", "mallory", **kwargs))
    with pytest.raises(ProcessDied):  # the headless client exits with the server's reason
        make_clients(server)(1, extra_args=["--auth-token-file", str(f)])
    expect(server).to_have_player_count(0)


def test_missing_token_never_joins(auth_server, make_clients, tmp_path):
    server = auth_server()
    with pytest.raises(ProcessDied):
        make_clients(server)(1, extra_args=["--auth-token-file", str(tmp_path / "does-not-exist")])
    expect(server).to_have_player_count(0)


def test_a_player_without_auth_support_is_refused(auth_server, make_clients, tmp_path, monkeypatch):
    server = auth_server()
    # Hide any real opener (a CI runner has xdg-open): the browser launch must fail, not sit
    # waiting for a sign-in nobody will complete.
    empty = tmp_path / "empty-path"
    empty.mkdir()
    monkeypatch.setenv("PATH", str(empty))
    with pytest.raises(ProcessDied):  # no --auth-token-file and no browser: fails closed, never anonymous
        make_clients(server)(1)
    expect(server).to_have_player_count(0)


def test_same_account_signing_in_again_kicks_the_older_session(idp, auth_server, make_clients, tmp_path):
    server = auth_server()
    factory = make_clients(server)
    f = token_file(tmp_path, idp.mint("sub-carol", "carol"))
    (first,) = factory(1, extra_args=["--auth-token-file", str(f)])
    expect(server).to_have_player_count(1)
    (second,) = factory(1, extra_args=["--auth-token-file", str(f)])
    expect(server).to_have_player_count(1)  # the newcomer is never refused; the old one is gone
    expect(server).to_have_player_login("sub-carol", name="carol")
    expect(second).to_be_joined()


def test_two_people_called_alex_get_unique_names(idp, auth_server, make_clients, tmp_path):
    server = auth_server()
    factory = make_clients(server)
    f1 = token_file(tmp_path, idp.mint("sub-1", "alex"), "t1.txt")
    f2 = token_file(tmp_path, idp.mint("sub-2", "alex"), "t2.txt")
    factory(1, extra_args=["--auth-token-file", str(f1)])
    factory(1, extra_args=["--auth-token-file", str(f2)])
    expect(server).to_have_player_count(2)
    expect(server).to_have_player_login("sub-1", name="alex")
    expect(server).to_have_player_login("sub-2", name="alex#2")


def test_browser_flow_with_pkce_over_a_loopback_redirect(idp, auth_server, make_clients, tmp_path, monkeypatch):
    """No token file: the client runs the real Authorization Code + PKCE flow. The engine writes
    the authorization URL to VB_AUTH_URL_FILE instead of opening a browser; this test is the browser."""
    idp.next_user = {"subject": "sub-dave", "name": "dave", "claims": {"email": "d@example.com"}}
    server = auth_server()
    url_file = tmp_path / "auth_url.txt"
    visited = {}

    def play_browser():
        deadline = time.time() + 30
        while time.time() < deadline:
            if url_file.exists() and url_file.read_text():
                # Follows the IdP's 302 to the client's 127.0.0.1 listener, like a real browser.
                with urllib.request.urlopen(url_file.read_text(), timeout=10) as r:
                    visited["status"], visited["body"] = r.status, r.read().decode()
                return
            time.sleep(0.05)

    t = threading.Thread(target=play_browser, daemon=True)
    t.start()
    factory = make_clients(server)
    monkeypatch.setenv("VB_AUTH_URL_FILE", str(url_file))
    (dave,) = factory(1)
    t.join(10)
    assert visited.get("status") == 200 and "Signed in" in visited["body"], visited
    expect(server).to_have_player_login("sub-dave", name="dave")
    # PKCE S256 was used end to end, and the engine put the server's nonce in the request.
    q = idp.auth_requests[-1]
    assert q["code_challenge_method"] == "S256" and q["nonce"]
    assert idp.token_requests[-1]["grant_type"] == "authorization_code"
    assert idp.token_requests[-1]["code_verifier"]


@pytest.mark.slow
def test_first_use_trust_prompt_asks_before_contacting_the_idp(idp, auth_server, make_clients, tmp_path,
                                                               monkeypatch):
    """A windowed client (real sign-in screen) meets this server + IdP pair for the first time: it must
    ask before anything is sent to the IdP. After Continue and a browser sign-in, a second client with the
    same config dir is not asked again and signs in silently with the saved refresh token."""
    if not __import__("os").environ.get("DISPLAY"):
        pytest.skip("needs a display (xvfb-run)")
    idp.next_user = {"subject": "sub-erin", "name": "erin"}
    server = auth_server()
    url_file = tmp_path / "auth_url.txt"
    monkeypatch.setenv("VB_AUTH_URL_FILE", str(url_file))
    factory = make_clients(server)

    (first,) = factory(1, via_menu=True, home="erin")
    first.menu_connect("127.0.0.1", server.port, name="erin")
    expect(first).to_have_auth(phase="choosing", needs_trust=True, timeout=20.0)
    first.screenshot(tmp_path / "trust_prompt.png")
    time.sleep(0.5)  # give a wrong client every chance to jump ahead
    assert not url_file.exists(), "the browser flow started before the player trusted the server"
    asked = idp.requests("auth") + idp.requests("token")
    assert asked == [], "the client contacted the IdP before the player agreed: %r" % asked
    with pytest.raises(Exception):
        first.signin("browser")  # not offered while the trust prompt is up

    first.signin("trust")
    expect(first).to_have_auth(phase="choosing", needs_trust=False)
    first.signin("browser")
    deadline = time.time() + 20
    while not (url_file.exists() and url_file.read_text()) and time.time() < deadline:
        time.sleep(0.05)
    with urllib.request.urlopen(url_file.read_text(), timeout=10) as r:  # play the browser
        assert r.status == 200
    expect(server).to_have_player_login("sub-erin", name="erin", timeout=20.0)
    first.close()

    url_file.unlink()
    (again,) = factory(1, via_menu=True, home="erin")
    again.menu_connect("127.0.0.1", server.port, name="erin")
    expect(again).to_be_joined()
    assert not url_file.exists(), "a trusted pair with a saved login opened the browser again"
    assert "refresh_token" in idp.grants()


def test_revoked_login_is_kicked_after_the_grace_period(idp, auth_server, make_clients, tmp_path):
    """Periodic live re-auth (auth.md §5.6): the client re-reads its token file for every
    re-auth, so withholding it models an IdP that no longer issues tokens (a revoked session)."""
    server = auth_server(reauth_interval=60, reauth_grace=10)
    f = token_file(tmp_path, idp.mint("sub-erin", "erin"))
    (erin,) = make_clients(server)(1, extra_args=["--auth-token-file", str(f)])
    expect(server).to_have_player_count(1)
    # Withhold the token: no valid answer to the server's first re-auth request (due within
    # interval 60 s +/- 10%), so within that plus the 10 s grace the server drops the player.
    f.unlink()
    expect(server).to_have_player_count(0, timeout=100)
