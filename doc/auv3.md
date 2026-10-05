# AUv3（macOS の音源プラグイン）

実機の端子をそのまま口にしてある。

| 口 | 実機で言うと |
|---|---|
| 出力 0「Main Out」 2ch | MAIN OUT L/R。PHONES と DIGITAL OUT にも同じ信号が出ている |
| 入力 0「A/D Input」 2ch | A/D INPUT。左が AD1、右が AD2（サンプリングと A/D の系統に入る） |
| MIDI 入 ケーブル 0 | MIDI IN A（パート 1-16） |
| MIDI 入 ケーブル 1 | MIDI IN B（パート 17-32） |
| MIDI 出「MIDI Out」 | MIDI OUT（SH7043 の SCI ch0）。firmware の返事が出てくる |

VST3 は MIDI 入力 2 本と A/D INPUT までで、MIDI OUT を出していない。
AUv3 には MIDI 出力の口があるので、そこは実機に合わせて足した。

音源そのものは VST3 と同じ `src/vst3/engine.h` の `engine` を使う。VST3 の型は
一つも出てこないので、`smu2000::plug` という別名で呼んでいる。

AUv2（`aumu`/`SMU2`/`Trbh`）とは種別を変えてある（`aumu`/`SMU3`/`Trbh`）。
名前も束の ID も別なので、両方入れても取り違えず、AUv2 で残した曲は
AUv2 を指し続ける。

```
make auv3           build/S-MU2000.app を作る（中に .appex が入る）
make install-auv3   ~/Applications へ複製する
make auv3-roms      ROM を拡張のアプリケーションサポートへ入れる
make auval3         auval で検査する（aumu SMU3 Trbh）
build/autest        .appex を通さずその場で試す道具
```

## 作りの地図

| | |
|---|---|
| `src/auv3/audio_unit.mm` | `AUAudioUnit` の中身。口・描き出し・状態の持ち帰り |
| `src/auv3/factory.mm` | `.appex` の入口（`NSExtensionPrincipalClass`） |
| `src/auv3/view_controller.mm` | 画面の口。貼るのは `panel_nsview.mm` の 1 枚 |
| `src/vst3/panel_nsview.mm` | パネルを貼った `NSView`。VST3・AUv2・AUv3 で共通 |
| `src/auv3/main_app.mm` | 器のアプリ。音は出さない。ROM のインストールもする |
| `src/roms_dir.h` | ROM ディレクトリに必要な物と、それを入れる方 |
| `src/auv3/autotest.mm` | その場で登録して口と音と画面を確かめる |
| `src/ui/midi_split.h` | MIDI OUT のバイト列を 1 メッセージずつに切る |
| `packaging/auv3-*.plist` | 器と拡張の Info.plist（種別 SMU3） |
| `packaging/auv3-*.entitlements` | 砂場の権利。拡張には JIT の権利も |

## 画面が出るかどうかは拡張の種類で決まる

AUv3 の画面は、ホストが `requestViewControllerWithCompletionHandler:` を呼んで
くれるかどうかで決まる。macOS では **それを頼みに来るかどうかを拡張の種類が
決めている**:

| `NSExtensionPointIdentifier` | principal class | ホストは画面を頼むか |
|---|---|---|
| `com.apple.AudioUnit` | `NSObject` + `AUAudioUnitFactory` | 頼まない |
| `com.apple.AudioUnit-UI` | `AUViewController` + `AUAudioUnitFactory` | 頼む |

音はどちらでも同じように出る。だから `com.apple.AudioUnit` のままだと「鳴るのに
画面が出ない」になり、こちら側の記録には「画面を頼まれた」の 1 行も残らない
（`requestViewController` が呼ばれないので）。決まりは Xcode の雛形
（`Audio Unit Extension.xctemplate` の `TemplateInfo.plist`）にそのまま書いてあり、
同じ機械に入っている AUv3（SC-55、GM synth）も画面を持つ方で登録されている。
`factory.mm` と `packaging/auv3-appex-Info.plist` はこの形にしてある。

種類を直すと、拡張の起動のされ方が変わる。システムの記録にこう出るようになる
（直す前は出ない）:

```
launchd  draining messages from com.tarboh.smu2000.auv3.au.viewservice
```

拡張は `NSViewServiceApplication` として、宿主（`com.apple.ViewBridgeAuxiliary`）
つきで立ち上がる。principal class が `AUViewController` なのはこのためで、
AU の工場（`createAudioUnitWithComponentDescription:error:`）も同じクラスが兼ねる
（Apple の雛形も 1 クラスで両方を兼ねている）。

**AU を作るのは拡張の XPC の糸なので、view に触るのは主の糸で。**
`AUViewController` の `loadView` は `preferredContentSize` を立てるので、非主糸で
組むと AppKit が例外を投げ、それが ExtensionFoundation の中まで抜けて拡張ごと
落ちる（症状は「AUv3 が開けない」）。`factory.mm` は AU を組んだあと
`dispatch_async(main)` で貼る。

## 画面は engine より先に片付けなければならない

`plug_view` は engine を参照で持っている。AUv3 の engine は AU
（`SMU2000AudioUnitV3` の `_engine`）のものなので、**AU が先に消えて画面が後から
消えると、画面の後始末が解放済みの engine に触る**。`plug_view::removed()` は
最初に `m_engine.card_flush()` を呼ぶので、そこが外からの錠叩きになる:

```
libc++abi: terminating due to uncaught exception of type std::__1::system_error:
  mutex lock failed: Invalid argument        → std::terminate → 拡張ごと落ちる
```

どちらを先に手放すかはホスト次第（auval が相手でも起きる）。落ちるのは view
サービスなので、ホストから見ると「画面を頼んだのに何も出ない」になる -- 種類を
直しただけでは足りなかった理由がこれ。

だから **画面（`SMU2000PanelView`）が AU を掴む**。掴んでいれば ARC が AU を
手放すのはこの view の `dealloc` が終わった後になり、`plug_view` → engine の順が
保証される。`make_panel_view` の `owner` がその掴む先で、AUv3 は自分の
`AUAudioUnit` を渡す。`build/autest --view` がこの順序をそのまま試す道具で、
直す前は上の `mutex lock failed` で落ちていた
（`画面  SMU2000ViewControllerV3  1400 x 360` が出るようになった）。

AUv2 の画面（`au/editor_mac.mm`）は engine を AU のハンドル越しに取るので同じ手が
使えない。AUv2 ではホストが AU のハンドルを持っているので、`owner` には `nil` を
渡す -- 同じ 1 枚を使いながら、掴む先だけが形ごとに違う。

## 気をつけるところ

**標本化周波数はホストに合わせる。** MU2000 は 44100Hz でしか動かないので、
`engine` が自前の窓関数付き sinc で変換する。先読みはしないので `latency` は 0。

**描き出しの中では確保も錠もしない。** 器は `allocateRenderResources` で取る。
`maximumFramesToRender` は 4096。

**MIDI は標本単位で効く。** 事象の位置で区間に割って `engine::fill()` を呼ぶ。
昔のバイト列（`AURenderEventMIDI`/`MIDISysEx`）と UMP の
`AURenderEventMIDIEventList`（MIDI 1.0）の両方を受ける。SysEx7 は組み立てて
から渡す。

**出力レベルは 1 サンプルずつ寄せる。** パネルのつまみが目標で、VST3・CLAP と
同じ 1/512 ずつの掛け方。一気に変えると音が跳ねる。

**MIDI OUT は音源の溜めを直に引いてはいけない。**
`engine::fill()` の中で `ui::driver::pump_out()` が先に引いてパネルの画面へ
渡してしまうので、後から `mu2000::midi_out_take()` を呼んでも空になっている。
`pump_out()` の echo から engine 内の輪へ写し、`engine::midi_out()` はそれを読む。

**起動は実時間で待つ。** ROM を読んで空回しするのは `engine` の別の
スレッドで、描き出した量とは関係が無い。起動前の描き出しは無音をすぐ返すので、
回すだけでは一瞬で終わり「鳴らないプラグイン」に見える。

## 起動は「器を用意するとき」に待つ

`fill()` は起動が終わるまで無音を返す。これだけだと、実時間より速く回す
ホスト（ファイルを鳴らすもの）では困る: ホストは待ってくれないので、
起動の数秒ぶんの壁時計の間に曲の十数秒ぶんを描き出してしまい、そこは丸ごと
無音になる。そこにあった音符は溜められたまま、遅れて一度に流れる。

だから **`allocateRenderResources` の中で `engine::wait_ready()` を呼んで、
起動が終わってから器を返す**。ここは実時間の糸ではないので待ってよい。
鳴り始めたときには機械はもう立ち上がっている。

## 初めて挿したときも待たせない（写しを焼いておく）

`make auv3 AUV3_ROMS=roms` は、ROM を入れたあとにその場で一度起動して、
その姿をバンドルへ焼き込む:

```
<appex>/Contents/Resources/bootcache/<鍵>.bin
```

`boot()` は「焼いてあるもの」→「自分で残したもの」の順に見る。
焼いてあれば、容器が空の状態で初めて挿したときでも数ミリ秒で立ち上がる。

焼くのは既定の起動（`midi_ready` で止める）と同じ姿で、鍵も同じ。
`render` や VST3 と違う姿から始めると、食い違ったときに切り分けられない。
最後まで回したければ `S_MU2000_BOOT_SECONDS` に秒数を渡す（鍵に混ざる）。

焼くときは空の HOME で走らせる。砂場の中のプラグインは NVRAM を持たない
（容器が空）なので、作る側に自分の設定が混ざると鍵が変わり、焼いた写しが
使われない。Makefile が `HOME=$$(mktemp -d)` でそれを避けている。

ROM を入れない既定のビルド（`make auv3`）は写しも焼かない。焼いた写しは
`<appex>/Contents/Resources/bootcache` に入るので、それも ROM と同じ扱いで
入れる・外すのを選ぶ（`AUV3_ROMS=none` は両方を消す）。配るのは ROM も写しも
無い束にして、初回だけ 5〜8 秒待つ承受能力として受け入れる。

## ROM は自分のアプリケーションサポートから読む

登録されるということは砂場に入るということで、`$HOME` は容器
（`~/Library/Containers/com.tarboh.smu2000.auv3.au/Data`）へすり替えられる。
`roms.txt` の指す先や、`/Library/Application Support` のような共有の場所には
届かない。

**ただし自分の容器の中のアプリケーションサポートには届く。** それは

```
~/Library/Containers/com.tarboh.smu2000.auv3.au/Data/Library/Application Support/S-MU2000
```

で、`log.txt` や起動の写しをすでに入っている場所そのものである。engine の
`config_dir()`（`src/compat/paths.h`）がこの中を指すので、**ここへ置いた ROM は
VST3 と同じ道で見つかる**。バンドルは見なくてよい。

器アプリ（`src/auv3/main_app.mm`）がその場所へ入れる。一度起動して窓の
「Install ROMs...」でフォルダを選ぶ。もし
`~/Library/Application Support/S-MU2000/roms` に既に揃っていれば、
「Install from Application Support」のボタンを押すだけでよい。窓なしでもできる:

```
open -a S-MU2000.app --args --install-roms /path/to/roms
make auv3-roms AUV3_ROMS=roms     # 同じことをスクリプトから
```

複製するのは engine が読むファイルだけ（36MB）で、波形 banks の余分は入れない。
一時名へ書いてから置き換えるので、途中で止めても半端な画像は残らない。

### なぜ器アプリは砂場に入っていないのか

容器は拡張の当中にある。**砂場に入ったプロセスは自分の容器にしか書けない**ので、
ファイルの置き先は拡張の容器でなければならない。器アプリが
`com.apple.security.app-sandbox` を持たないのはそのため
（`packaging/auv3-app.entitlements`）。

拡張側の権利書はそのまま（登録には砂場が要る、次節）。器アプリは何も自分の
バンドルの外へ書かないので、砂場のないことの代償はない。

### 開発中はバンドルに入れてよい

```
make auv3 AUV3_ROMS=roms
```

`<appex>/Contents/Resources/roms` に複製される。署名より前に入れること
（後から足すと封が破れる）。既定（AUV3_ROMS は空）では入れないので、配る
bundle は ROM を持たない。入れられるのは開発を楽にするためだけで、
配ることには不利に働く: **バンドルに ROM が入っているビルドは誰にも配れない**。

探す順番は「環境変数 → アプリケーションサポート → バンドルの隣 → 機械共通」
（`src/vst3/engine.cpp`）。app support を先に聞くので、両方あるときは
自分のものを採る。「ROM の揃った場所か」は 4MB のプログラム ROM と 8MB の
波形 ROM 四つを**まとめて**見るので、半端にコピーされた場所があっても
別の場所を探しに行く（engine は最初の一つで止まるため）。

## 登録には App Sandbox の権利が要る

macOS の app extension は砂場に入っていないと登録されない。
権利書を付けずに署名すると、次のように「もう少しで動きそう」な状態になり、
何が悪いのか分からない:

* LaunchServices はアプリも拡張も見えている
* 署名は正しい（`codesign --verify --deep --strict` が通る）
* それでも `pluginkit` にも `auval -a` にも出てこない

足りなかったのは `com.apple.security.app-sandbox` だけだった
（`packaging/auv3-appex.entitlements`）。署名するときに `--entitlements` で渡す。
証明書の種類は関係がない（ad-hoc でも登録される）。プロビジョニングプロファイル
も要らなかった。

**Needs the sandbox on the extension, not on the container app.** 前節のとおり
器アプリは ROM を拡張の容器へ入れるので砂場に入れてはいけない
（`packaging/auv3-app.entitlements` は空）。器アプリが砂場に入っていても
拡張の登録自体はそのまま通るので、動かないのは器アプリ側だけ。

SH2 と MEG の JIT は `MAP_JIT` で写像する。堅めの実行環境では
`com.apple.security.cs.allow-jit` が無いと JIT が通らず通訳に落ちる
（拡張の権利書に入れてある）。

## 確かめたこと

`build/autest` は `.appex` を通さず、`+[AUAudioUnit registerSubclass:]` で
その場に登録して鳴らす（`--view` で画面、`--sysex-ump` で UMP の SysEx も）:

```
口:
  出力 0  Main Out     2 ch
  入力 0  A/D Input    2 ch
  MIDI 出   MIDI Out
  MIDI 入   ケーブル 2 本
標本化周波数 48000 Hz / 遅れ 0.00 ms

MIDI IN A（ケーブル 0 → パート 1）  peak 0.0748  鳴った
MIDI IN B（ケーブル 1 → パート 17） peak 0.0744  鳴った
MAIN OUT   peak 0.0748  rms 0.01596（143360 フレーム / 48000 Hz）
A/D INPUT  引かれた回数 640
MIDI OUT   1 メッセージ / 15 バイト  （識別要求に返事が来た）
画面  SMU2000ViewControllerV3  1400 x 360
```

`auval -v aumu SMU3 Trbh` も通る（**AU VALIDATION SUCCEEDED**）。
22050 / 44100 / 48000 / 96000 / 192000 Hz、64〜4096 フレーム、
細切れの描き出し、MIDI、どれも PASS。

なお v3 の拡張は `AudioComponentFindNext()` には出てこない。
探すときは `AVAudioUnitComponentManager` を使う。

**`--system` は署名の無い道具からは通らない。** `build/autest --system` は
システムに登録済みの `.appex` を別プロセスで掴もうとするが、システムが受け皿
（`com.apple.audio.AUHostingService`）を起こさないので、拡張は起動するのに AU が
返ってこない（`作れない: (理由なし)` と出て 30 秒待つ）。auval では起こる。
入れたあとの確認は auval で（`make auval3`）。その場で試すのは
`build/autest --view`（拡張を通さない道）。
