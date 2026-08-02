# Changelog

## [1.2.2](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v1.2.1...tdlib-purple-v1.2.2) (2026-08-02)


### Bug Fixes

* preserve release package permissions ([b7ea34b](https://github.com/adrighem/tdlib-purple/commit/b7ea34b0b34662fbc82246515305dc9e26516a6a)), closes [#9](https://github.com/adrighem/tdlib-purple/issues/9)

## [1.2.1](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v1.2.0...tdlib-purple-v1.2.1) (2026-08-02)


### Bug Fixes

* require Telegram credentials for every build ([fb10394](https://github.com/adrighem/tdlib-purple/commit/fb10394f707cec725fad724aaa479b969c3c1c8e)), closes [#9](https://github.com/adrighem/tdlib-purple/issues/9)
* simplify Telegram plugin name ([4d14778](https://github.com/adrighem/tdlib-purple/commit/4d14778ea1f9cab017406e2b730d5a8edcbcb9fc))

## [1.2.0](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v1.1.1...tdlib-purple-v1.2.0) (2026-08-01)


### Features

* adapt TDLib forum topic metadata ([343f2d6](https://github.com/adrighem/tdlib-purple/commit/343f2d6a68f7fe99264bd68ea49b48b906b9995e)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* add forum topic registry ([8522c7f](https://github.com/adrighem/tdlib-purple/commit/8522c7ff2cb73f0599a687d068c9cdba2e31919e)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* add forum topic room identity ([e560dfb](https://github.com/adrighem/tdlib-purple/commit/e560dfb3bf33bbd7a876d536f7c5c74c7e369353)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* bootstrap native Purple 3 plugin ([220f09c](https://github.com/adrighem/tdlib-purple/commit/220f09c7d99c6979f9be11288612bba3e0b5f059)), closes [#9](https://github.com/adrighem/tdlib-purple/issues/9)
* complete Telegram topic read state and lifecycle ([a85e95f](https://github.com/adrighem/tdlib-purple/commit/a85e95f3e1577d6819838e0add7925b2df4fc74b)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* derive exact Telegram message targets ([f40efd1](https://github.com/adrighem/tdlib-purple/commit/f40efd17f0bc79c056ceabd7a71e8ae014547fae)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* discover Telegram forum topics ([51f1fa1](https://github.com/adrighem/tdlib-purple/commit/51f1fa1538937fa27977ff8cee3893a0bfe7cf51)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* open exact Telegram forum topics ([82fa9dc](https://github.com/adrighem/tdlib-purple/commit/82fa9dceda29f7553e904cdf04b2a139f9e1756d)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* project Telegram forum roster state ([b7dd6ca](https://github.com/adrighem/tdlib-purple/commit/b7dd6ca91e8d8f16630d1089374142bbb1cb36e5)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* project Telegram topic lifecycle ([b612589](https://github.com/adrighem/tdlib-purple/commit/b6125893bc8b1ab83bb8f07762a26c79a622ffe9)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* **purple3:** add connection settings foundation ([edff16f](https://github.com/adrighem/tdlib-purple/commit/edff16fb7cbedf1f84b3f6cd30994f83725492fa)), closes [#9](https://github.com/adrighem/tdlib-purple/issues/9)
* route incoming Telegram forum topics ([8417166](https://github.com/adrighem/tdlib-purple/commit/8417166fce056ab93d7dde1bf56040e883409d3c)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* route Telegram message updates by topic ([b8ecb2b](https://github.com/adrighem/tdlib-purple/commit/b8ecb2b2b23d1cfbc43fed41579d112857896185)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* route Telegram sends by topic ([5ad74b7](https://github.com/adrighem/tdlib-purple/commit/5ad74b7332759da80ddbb9e395be81d352466b25)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* route Telegram uploads by topic ([183e1a4](https://github.com/adrighem/tdlib-purple/commit/183e1a46f74f002b35db13d8f50c47fbf2338a8e)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)


### Bug Fixes

* **auth:** prevent sensitive authentication diagnostics ([f72af9a](https://github.com/adrighem/tdlib-purple/commit/f72af9af6ae34d8cb63c63b74fc820bac21e2c7c)), closes [#9](https://github.com/adrighem/tdlib-purple/issues/9)
* **auth:** restart onboarding on persistent API ID error ([e9ef3eb](https://github.com/adrighem/tdlib-purple/commit/e9ef3eb3aecf5a3874f59d030f569e6856696571)), closes [#11](https://github.com/adrighem/tdlib-purple/issues/11) [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* avoid logging authentication codes ([bc38bb6](https://github.com/adrighem/tdlib-purple/commit/bc38bb621b2584d32358779569416dedd02f1a69))
* detach destroyed test transceivers ([5e3e991](https://github.com/adrighem/tdlib-purple/commit/5e3e991577f48dce54152ed9fed1bf04fd2d8ae8)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* **forum:** alias added topic rooms immediately ([f243a54](https://github.com/adrighem/tdlib-purple/commit/f243a5404756373e76e4335534d60f9d2ee62255)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* harden asynchronous Telegram topic delivery ([69b4532](https://github.com/adrighem/tdlib-purple/commit/69b4532d9ab368602af8c672ae7ce88a5e562389)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* harden Telegram topic lifecycle races ([55bb463](https://github.com/adrighem/tdlib-purple/commit/55bb4637c94e5c0f35d99288aa4186ca2d753560)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* make query timeouts reentrancy-safe ([a5fa8e3](https://github.com/adrighem/tdlib-purple/commit/a5fa8e3a5dfc4371ad2dfb40026b158c6598e1ae)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* provide safe Telegram display names ([449b196](https://github.com/adrighem/tdlib-purple/commit/449b1960112f92f2ac4ba2142960955d735a5733)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* **purple3:** make account setup QR-ready ([cb04ba6](https://github.com/adrighem/tdlib-purple/commit/cb04ba6dd23a2ac2d252121330d8519fa1871fec)), closes [#9](https://github.com/adrighem/tdlib-purple/issues/9)
* **purple3:** release failed account connections ([8638925](https://github.com/adrighem/tdlib-purple/commit/8638925528d8d63ba06e02c422b559f221379095)), closes [#9](https://github.com/adrighem/tdlib-purple/issues/9)
* report failures in unavailable topic rooms ([b2dc313](https://github.com/adrighem/tdlib-purple/commit/b2dc31349c80a97082a83f91139c7eb0c4f8d65c)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)
* restart invalidated topic metadata lookups ([f79cceb](https://github.com/adrighem/tdlib-purple/commit/f79ccebf84e7727a15042df496bbf6fb65aadbdb)), closes [#8](https://github.com/adrighem/tdlib-purple/issues/8)

## [1.1.1](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v1.1.0...tdlib-purple-v1.1.1) (2026-07-22)


### Bug Fixes

* quote original messages for unread reactions ([a2e62d5](https://github.com/adrighem/tdlib-purple/commit/a2e62d5b172ea1f07e406b5cc08a66ee4a480601))

## [1.1.0](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v1.0.0...tdlib-purple-v1.1.0) (2026-06-29)


### Features

* upgrade TDLib and align rich text handling ([35a8776](https://github.com/adrighem/tdlib-purple/commit/35a8776f440aa23f97a0a977a6012a0e1af64001))

## [1.0.0](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v0.9.2...tdlib-purple-v0.10.0) (2026-06-24)


### Features

* bridge tdlib features into libpurple ([e7ab6e6](https://github.com/adrighem/tdlib-purple/commit/e7ab6e6338a4098f6cd7f590fdd394c71ae5ccee))


### Bug Fixes

* limit outgoing text URL entities ([29b9102](https://github.com/adrighem/tdlib-purple/commit/29b9102cb6ec6148540b24b4484c613ea6414466))

## [0.9.2](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v0.9.1...tdlib-purple-v0.9.2) (2026-06-21)


### Bug Fixes

* preserve UTF-8 buddy server aliases ([0513a28](https://github.com/adrighem/tdlib-purple/commit/0513a28b3f10fc5a8bfc7f735c94dfc6ca7fe8a4))

## [0.9.1](https://github.com/adrighem/tdlib-purple/compare/tdlib-purple-v0.9.0...tdlib-purple-v0.9.1) (2026-06-20)


### Bug Fixes

* avoid narrow integer multiplication in image buffers ([b91056a](https://github.com/adrighem/tdlib-purple/commit/b91056a6b050d36e30b3f83f3652c98b0483d111))
