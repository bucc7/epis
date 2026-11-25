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
and minimize. Key bindings use Control as written (`<C-w>v`); Command is left
to the system.

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
| `d` `a` `m` `s` `u` | delete, archive, move, flag, toggle read |
| `<C-w>v` `<C-w>s` `<C-w>hjkl` `<C-w>q` `<C-w>o` | window management |
| `<leader>c` `<leader>s` | compose, synchronize |
| `<leader>a`, `:ask`, `:ai_draft` | summarize, ask, or draft a reply with the LLM |
| `:` | command line (`:move Archive`, `:attach FILE`, `:send`, `:w`, `:q`, `:!cmd %f`) |

The mode name in the status bar has a colour for each mode, set by the
`mode_normal`, `mode_insert`, `mode_visual`, `mode_vline`, `mode_command`,
`mode_find` and `mode_setup` theme keys. If unset, normal and setup follow
`accent` and `mode_vline` follows `mode_visual`.

The composer uses vim keys by default (`vim_edit = no` turns them off). It
supports motions, counts, operators (`d`, `c`, `y`) with motions, visual
mode, `p`, `u`, `r`, `J`, `o`, and `ZZ` or `:send` to send.

You can rebind every key in `[keys]` and define your own commands in
`[commands]`. A command can chain others with `;`, run a shell command with
`!`, or replay keys with `feedkeys`. Colours, corner radius and padding are
set in `[theme]`, and `include` loads a theme file (see `themes/`).

## Design notes

- **Redraws:** the UI is not drawn in immediate mode. epis redraws only on
  input, resize or when a background job finishes, and sleeps in `poll(2)`
  otherwise. It uses no CPU while idle.
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
- GSSAPI/Kerberos and NTLM authentication.
- Fractional scaling on Wayland (integer scale plus the `scale` option),
  client-side decorations on compositors without xdg-decoration (GNOME),
  and the system clipboard.
