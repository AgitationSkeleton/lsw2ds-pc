# TURN relay for CGNAT and symmetric NAT

> **SUPERSEDED, and kept as a design rather than a deployment. Read this first.**
>
> Everything below describes the **Godot port**, which this project no longer
> is. Verified against the current tree:
>
> - the client's transport is plain TCP - `port/net/lsw2_tcp.h` - with UPnP for
>   traversal (`lsw2_upnp.h`). There is **no WebRTC, no ENet and no ICE anywhere
>   in `port/`**;
> - **nothing in the client ever requests `/v1/turn-credentials`**. The master
>   worker still mints those credentials, and no code consumes them;
> - `tools/vendor/godot` is gone - the Godot export templates were deleted as
>   dead weight in the same sweep that took the repository from 27 GB to 10 GB.
>
> So this relay, as written, **cannot carry the current game's traffic**. Bringing
> it back would mean either giving the C++ client an ICE transport, or replacing
> this with a plain TCP relay that the existing `Lsw2TcpTransport` can dial -
> a much smaller job, and the one that fits what the client already speaks.
>
> The master-server design does not call for a relay at all, so nothing is
> unfulfilled by leaving this parked. It is kept because the coturn
> configuration and the credential-minting scheme are still sound and would be
> reusable; it is marked because a deployment template that silently describes
> the wrong architecture is worse than none.

The desktop Godot 4.7.1 build bundled in `tools/vendor/godot` exposes both
`WebRTCPeerConnection` and `WebRTCMultiplayerPeer`. The relay path therefore
uses WebRTC ICE with coturn instead of attempting to proxy ENet datagrams.

This directory is a deployment template, not a live public relay. Copy
`turnserver.conf` to a host-local ignored configuration and replace the
placeholder realm and shared secret, set the public IP as
coturn's `external-ip` when the host is behind one-to-one NAT, and open UDP/TCP
3478 plus UDP 49160-49200. Add a certificate and key before enabling production
TLS on 5349. Never commit the real secret; `.dev.vars` and `.env` are ignored.

Coturn's shared-secret mode expects a temporary username
`expiration:user-id` and a base64 HMAC-SHA1 password. The master Worker endpoint
`GET /v1/turn-credentials` mints those for five minutes. Configure the Worker
secrets `TURN_SHARED_SECRET`, `TURN_URL`, and optionally `STUN_URL`; configure
coturn with the identical shared secret.

Run `docker compose config` to validate locally and `docker compose up -d` on a
public dual-stack Linux host. Verify allocations using coturn's
`turnutils_uclient` from a network other than the relay host. The game must use
the WebRTC multiplayer transport when `relay_required` is true; ENet remains
the direct/UPnP transport.
