# epis - mail client

## Description
epis is a professional, bloat-less mail client, made to replace Thunderbird as
that piece of software is really bloated.
It follows the UNIX philosophy of doing one thing and doing it well.

## Implementation
epis is implemented completely in pure pedantic C11, using the Clay library for
layout and Vulkan for rendering on Linux and Windows and Metal on macOS (the macOS
and Windows implementations are left for later as the priority is Linux) and is
distributed under the MIT license. The most important thing is that the codebase
MUST BE SMALL! The implementation must be feature complete but simple, without
unnecessary abstractions. epis must be AI-ready, featuring an headless CLI mode
for agents to read emails and send them, with optional integration for LLMs using
API keys provided by the user. The only build system used to build epis is make, with
additional python scripts if really needed (for things like shader compilation if it
can't be efficiently and cleanly done with make). A config.mk file is provided with
general compilation options so that the user can configure their preferences on what
parts of the program are compiled to fit their specific needs.
### UI/UX
The UI must use the lightweight Clay library for layout and the Linux implementation
should use Vulkan as well as the Windows one for rendering the widgets. Text rendering
can be done using freetype2 or harfbuzz (whatever is more lightweight). Windowing must
be implemented using only system libraries (xcb, wayland libraries, win32, Cocoa) and
Wayland support must be a primary concern for Linux, with X11 support as well (the
windowing backend must be a compilation option for Linux builds). There must be a
menu-bar and the progam is controlled using VIM-like commands and keybindings
(that should be configurable by the user). On macOS and when using something
like KDE global menus, that one menu bar should be used instead of rendering one
inside the window. There must be extensive support for theming through configuration
files. To navigate inboxes, pressing SPACE+fi will open the "find inbox" menu, which
is basically Telescope.nvim to find inboxes. Pressing SPACE+fm opens the "find message"
menu to find a specific message that can be fuzzy-found by typing either the object
or the address of the sender. Similar fuzzy-finding menus should drive the user experience
through the whole program. A list-view with all the messages contained in the currently
opened mailbox is also present so that messages can be selected with either hjkl,
positional arrows or the mouse. Clicking on a message (or pressing enter while it's
selected) opens the message view in the current window (the term window here is used to
refer to VIM-like windows, so something internal to the main window) and can be opened
on a new window by clicking on an entry in the context menu shown after right clicking
on it (or by pressing SHIFT+enter while the message is selected). VIM-like window management
should be implemented (similarily to how a tiling window manager works) but vertical windows
(side by side) should be default instead of being one on top of the other. A saner leader key
should be used by default, like SPACE instead of CTRL but it must be configurable by the user.
Message editing must also implement the VIM mode by default but it should be toggable from
configuration. Keybindings for general mail management should be present like synching messages,
sending them and so on. Configurable actions with custom keybindings should also be possible
(by using vim-like commands with `:`).
On first run, if a configuration file is not present, the user should be guided through a basic
configuration of the experience.

### Coding style
For code, follow the Linux coding convention and use the `indent` command for formatting.
Use `/* */` for comments and multiline comments look like this:
```
/*
 * this is a
 * multiline
 * comment.
 */
```
The code should be clear by itself so don't put comments about how things work but rather
on what functions do.

### Authentication
All the currently used authetication methods for IMAP and SMTP should be implemented. Use OpenSSL
for TLS and also add OAuth2.0 for services like Outlook and Gmail. Automatic configuration discovery
should also be implemented but do it all in this source code, don't use external libraries for this
kind of things.

### Performance
The whole thing must use as few resources as it can so for text editing and such consider using efficient
data structures like piece tables or gaps. Which one to decide depends on experiments done to see what is
most efficient.
Don't draw the UI in immediate mode, only redraw when needed so that less power is consumed on mobile devices
like laptops.

### Mail storage
CLI commands to download emails from IMAP should be provided so that the user can set up a cronjob or similar
things to synchronize their inboxes when needed. Mails should be stored using the Maildir format, in a configurable
directory with a sane default.

### Commits
Commits on git should follow the "conventional commits" convention which is the following:
```
The key words “MUST”, “MUST NOT”, “REQUIRED”, “SHALL”, “SHALL NOT”, “SHOULD”, “SHOULD NOT”, “RECOMMENDED”, “MAY”, and “OPTIONAL” in this document are to be interpreted as described in RFC 2119.

1. Commits MUST be prefixed with a type, which consists of a noun, feat, fix, etc., followed by the OPTIONAL scope, OPTIONAL !, and REQUIRED terminal colon and space.
2. The type feat MUST be used when a commit adds a new feature to your application or library.
3. The type fix MUST be used when a commit represents a bug fix for your application.
4. A scope MAY be provided after a type. A scope MUST consist of a noun describing a section of the codebase surrounded by parenthesis, e.g., fix(parser):
5. A description MUST immediately follow the colon and space after the type/scope prefix. The description is a short summary of the code changes, e.g., fix: array parsing issue when multiple spaces were contained in string.
6. A longer commit body MAY be provided after the short description, providing additional contextual information about the code changes. The body MUST begin one blank line after the description.
7. A commit body is free-form and MAY consist of any number of newline separated paragraphs.
8. One or more footers MAY be provided one blank line after the body. Each footer MUST consist of a word token, followed by either a :<space> or <space># separator, followed by a string value (this is inspired by the git trailer convention).
9. A footer’s token MUST use - in place of whitespace characters, e.g., Acked-by (this helps differentiate the footer section from a multi-paragraph body). An exception is made for BREAKING CHANGE, which MAY also be used as a token.
10. A footer’s value MAY contain spaces and newlines, and parsing MUST terminate when the next valid footer token/separator pair is observed.
11. Breaking changes MUST be indicated in the type/scope prefix of a commit, or as an entry in the footer.
12. If included as a footer, a breaking change MUST consist of the uppercase text BREAKING CHANGE, followed by a colon, space, and description, e.g., BREAKING CHANGE: environment variables now take precedence over config files.
13. If included in the type/scope prefix, breaking changes MUST be indicated by a ! immediately before the :. If ! is used, BREAKING CHANGE: MAY be omitted from the footer section, and the commit description SHALL be used to describe the breaking change.
14. Types other than feat and fix MAY be used in your commit messages, e.g., docs: update ref docs.
15. The units of information that make up Conventional Commits MUST NOT be treated as case-sensitive by implementors, with the exception of BREAKING CHANGE which MUST be uppercase.
16. BREAKING-CHANGE MUST be synonymous with BREAKING CHANGE, when used as a token in a footer.
```
