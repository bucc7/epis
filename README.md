# epis

epis is a small mail client written in C11. It syncs IMAP mailboxes to
Maildir, sends mail over SMTP and offers two interfaces on the same core:

- a graphical client that you drive with vim-style keys, laid out with
  [Clay](https://github.com/nicbarker/clay) and drawn with Vulkan on Wayland
  or X11, and with Metal on macOS;
- a headless command line with JSON output, for cron jobs, scripts and AI
  agents.

It is distributed under the MIT license.

## Build

Requirements: a C11 compiler, GNU make, OpenSSL and FreeType. The graphical
client also needs, on Linux, `glslc`, xkbcommon, the Vulkan loader and
headers, and either wayland-client, wayland-cursor and wayland-protocols or
xcb and xkbcommon-x11. On macOS it needs only the system frameworks and the
Xcode command line tools:

    xcode-select --install
    brew install pkgconf openssl@3 freetype

Then build:

    make                 # Cocoa on macOS, Wayland on Linux
    make WINDOW=x11      # X11 build
    make GUI=0           # headless CLI only, needs just OpenSSL
    make install         # PREFIX=/usr/local by default

`config.mk` holds the compile-time options. `make format` runs `indent
-linux` over the sources.

## macOS

The menus are in the system menu bar, so none is drawn in the window and the
`menu` and `menubar` commands do nothing. Cmd-Q, Cmd-H and Cmd-M quit, hide
and minimize, and the Edit menu has the usual Cmd-C, Cmd-X, Cmd-V and Cmd-A.
Key bindings use Control as written (`<C-w>v`); Command is left to the
system.

Option types characters, as it does elsewhere on macOS, so layouts that need
it for `@`, `[` or `{` work. It counts as Alt (`<A-...>`) only together with
Control or with a key such as an arrow or Return. Control-click is a right
click.

The configuration and token paths are the same as on Linux. The Metal shader
is compiled when epis starts, so the Metal toolchain is not needed to build.
`make` installs a plain binary, not an application bundle; start it from a
terminal.

## First run

Running `epis` without a configuration opens a setup form. It discovers the
server settings for your address using, in order: the provider's autoconfig
file, the Thunderbird ISPDB, your MX provider's ISPDB entry, and DNS SRV
records. If none of those work it guesses `imap.domain` and `smtp.domain`.
`epis setup` runs the same setup in the terminal. See `epis.conf.example`
for every option.

For Gmail and Outlook, register an OAuth client with the provider, set
`oauth` and `oauth_client_id`, then run `epis oauth ACCOUNT` once.

## Command line

    epis sync                          # cron friendly two-way sync
    epis list --unread --json          # newest first, with stable ids
    epis read test/INBOX/1795...,U=42  # prints and marks seen
    echo hi | epis send --to bob@example.com --subject Hello
    echo thanks | epis reply --send ID
    epis llm summarize ID              # needs [llm] api key

Run `epis help` for the full list, or see `man epis`.

## Graphical client

The leader key is Space by default (`leader` option).

| keys | action |
| --- | --- |
| `<leader>fi` | find an inbox (fuzzy finder) |
| `<leader>fm` | find a message by subject or sender |
| `<leader>fc` | find a command |
| `j` `k` `gg` `G` `<C-d>` `<C-u>` | move in the list or scroll a message |
| `<CR>`, click | open in the current window |
| `<S-CR>`, right click | open in a new window / context menu |
| `r` `R` `f` | reply, reply all, forward |
| `dd` | delete (in the list and in a message) |
| `a` `m` `s` `u` | archive, move, flag, toggle read |
| `v` `V` in the list | select several messages |
| `<C-w>v` `<C-w>s` `<C-w>hjkl` `<C-w>q` `<C-w>o` | window management |
| `<leader>c` `<leader>s` | compose, synchronize |
| `<leader>a`, `:ask`, `:ai_draft` | summarize, ask, or draft a reply with the LLM |
| `v` `V`, or drag with the mouse | select text in a message |
| `y` | copy the selection |
| `<C-S-c>` `<C-S-x>` `<C-S-v>` | copy, cut, paste, in any mode and field |
| `:` | command line (`:move Archive`, `:attach FILE`, `:send`, `:w`, `:q`, `:!cmd %f`) |

### Managing messages

The message list takes counts, a delete operator and a visual mode.

| keys | effect |
| --- | --- |
| `5j` `12G` | move five rows, go to row twelve |
| `dd` `3dd` | delete this message, or three from here |
| `dj` `dk` `d2j` `dG` `dgg` `d5G` | delete from here to where the motion goes |
| `3a` `3s` `3u` `3m` | archive, flag, toggle read or move three messages |
| `v` or `V` | select; extend with any list motion, `o` swaps ends |
| `d` `x` `a` `s` `u` `m` | act on the selection |
| `<Esc>` | drop the selection, a count or a waiting `d` |
| `:2,8delete` `:%archive` `:.,$move Junk` | act on a range of rows |

Typing `:` over a selection starts the command with `'<,'>`, and your own
commands and key bindings act on the selection too. Flagging or toggling a
mixed selection sets it on all of them first. Right-click inside a
selection to use the menu on all of it.

Delete moves messages to the trash folder. Inside the trash, or without a
trash folder, it only marks them; they are shown dimmed and the next sync
removes them for good. There is no undo in the list, and a sync applies
deletions and moves to the server.

If mail arrives while you are selecting and the selection would then cover
different messages, it is cleared. A sync or other background command that
finishes also cancels a half typed command such as `d`.

In `[keys]`, `delete_motion` is the command behind the `d` of `dd`. To go
back to deleting with one key, bind `mail d = delete`.

In a message, `v` and `V` start a selection. The usual motions extend it,
`o` swaps its ends, and `<Esc>` drops it. A selection can span the headers
and the body. On macOS, Cmd-C, Cmd-X, Cmd-V and Cmd-A work too. With
`clipboard = yes` (the default), text you yank in the composer goes to the
system clipboard and `p` pastes what you copied elsewhere. Deleted text
stays in the editor, so `dd` does not overwrite the clipboard.

The mode name in the status bar has a colour for each mode, set by the
`mode_normal`, `mode_insert`, `mode_replace`, `mode_visual`, `mode_vline`,
`mode_vblock`, `mode_command`, `mode_find` and `mode_setup` theme keys. If
unset, normal and setup follow `accent`, and `mode_vline` and `mode_vblock`
follow `mode_visual`. `search` colours search matches.

### The editor

The composer uses vim keys by default (`vim_edit = no` turns them off). The
engine is epis's own and was checked against Neovim, with classic Vim
defaults (`Y` yanks a line, `startofline`, `nojoinspaces`).

| area | supported |
| --- | --- |
| modes | normal, insert, replace, visual (`v` `V` `<C-v>`), command line, search |
| motions | `hjkl` `w b e ge` `W B E gE` `0 ^ $ g_ \|` `gg G H M L` `f F t T ; ,` `% { } ( )` `+ - _` `gj gk g0 g$`, counts and `N%` |
| operators | `d c y > < gu gU g~ gq gw`, doubled for lines, with any motion or text object |
| text objects | `iw aw iW aW is as ip ap`, quotes, `() [] {} <>` (`b`, `B`) |
| editing | `x X s S r R D C Y J gJ p P gp gP o O i I a A gI ~ <C-a> <C-x>` |
| visual | all operators, `o`, `r`, `J`, `p`, `gv`, block `I A c d y p r > <` |
| repeat | `.`, unlimited `u` and `<C-r>` |
| macros | `q{reg}`, `@{reg}`, `@@`, `@:`, counts; recursive macros stop at the first failing command |
| registers | `"a`-`"z` (`"A` appends), `"0`-`"9`, `"-`, `"_`, `"+`, `"*`, `"/` |
| marks | `m{a-z}`, `'` and `` ` `` jumps, `'<` `'>` and `''` |
| search | `/ ? n N * # g* g#` with Vim-style patterns, smart case, highlighting |
| insert mode | `<C-w> <C-u> <C-r>{reg} <C-o> <C-t> <C-d>`, arrows |
| scrolling | `<C-d> <C-u> <C-f> <C-b> <C-e> <C-y> zz zt zb` |
| commands | ranges with `:s` `:&` `:g` `:v` `:normal` `:d` `:y` `:pu` `:j` `:m` `:t` `:>` `:<` `:sort` `:set` `:noh` and line numbers |

Other `:` commands are epis commands, so `:w` saves a draft, `:x` or `ZZ`
sends, `:q!` discards the message and `:attach` adds a file. `gq` refills
paragraphs to `textwidth` (72) and keeps each quoting level apart.

Long lines wrap on screen. Set `number = yes` and `relativenumber = yes` for
line numbers; with both, the current line shows its own number and the
others their distance from it. `:set nu`, `:set rnu` and their `no` forms
change them while epis runs.

A few things differ from Vim. Marks follow the text when it is edited.
Search patterns are translated to POSIX regular expressions, so `\{-}`,
look-around and patterns spanning lines do not work. A macro stops if
another window takes the focus, so its remaining keys cannot act on mail.
Not implemented: folds, the jump and change lists (`<C-o>`, `<C-i>`, `g;`),
completion, tag text objects, `=`, `:s` with the `c` flag, and Vim script.

In a message, the same motions, text objects and searches work while text
is selected.

You can rebind every key in `[keys]` and define your own commands in
`[commands]`. A command can chain others with `;`, run a shell command with
`!`, or replay keys with `feedkeys`. Colours, corner radius and padding are
set in `[theme]`, and `include` loads a theme file (see `themes/`).

## Design notes

- **Redraws:** the UI is not drawn in immediate mode. epis redraws only on
  input, resize or when a background job finishes, and sleeps in `poll(2)`
  otherwise. It uses no CPU while idle.
- **Undo:** the editor logs each change instead of copying the text, so
  undo is unlimited and costs memory only for what changed.
- **Text buffer:** the editor uses a gap buffer. In a benchmark of 400,000
  typing and deleting edits in bursts at random positions in a ~500 KB
  document, the gap buffer took 0.02 s and 0.8 MB. An array-backed piece
  table took 2.0 s and 2.2 MB, and reading the whole text back was about
  5000 times slower. Emails are small and edits are local, so the gap
  buffer wins on speed, memory and code size.
- **Background work:** sync and LLM requests run as child `epis` processes,
  so the UI never blocks on the network and needs no threads.
- **Sync:** IMAP sync merges flags three ways against the last synced state
  stored in `.epis-state`. Local deletions, moves and new messages (such as
  sent copies and drafts) are uploaded on the next sync. File names carry
  the UID as `,U=<uid>`, as with mbsync.
- **Authentication:** PLAIN, LOGIN, CRAM-MD5, SCRAM-SHA-1, SCRAM-SHA-256,
  XOAUTH2 and OAUTHBEARER, plus the IMAP `LOGIN` command. OAuth2 uses the
  authorization code flow with PKCE and a loopback redirect.
- **LLM:** the `llm` commands call the Claude Messages API (default model
  `claude-opus-5-5`) with a key you provide.

## Not implemented yet

- The Windows (win32/Vulkan) backend; the spec defers it.
- The KDE global menu bar (dbusmenu): on Linux the menu is always drawn
  inside the window.
- Dead keys and input methods: no backend composes them.
- A macOS application bundle (`.app`).
- Selecting text with the mouse in the composer (use vim visual mode or
  select all), and the X11/Wayland primary selection.
- GSSAPI/Kerberos and NTLM authentication.
- Fractional scaling on Wayland (integer scale plus the `scale` option) and
  client-side decorations on compositors without xdg-decoration (GNOME).
