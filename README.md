# warframe-hub-server

Source-available implementation of a Warframe HUB server (used for relays & dojos).

## Usage

In general, it should just work. However, the HUB server does not support DTLS, so if you have a non-zero DTLS level configured, only loopback/localhost connections will work. For Bootstrapper clients, you can ask them to use UDP proxy on localhost, e.g. assuming your HUB server is reachable by your clients at `10.0.0.69`:
```json
{
  ...
  "dtls": 99,
  "hubAddress": "127.0.0.1:6951",
  "tunables": {
    "udpProxyUpstream": "10.0.0.69:6952"
  },
  ...
}
```
