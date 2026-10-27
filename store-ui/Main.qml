// Field Pay — the residency's money, as plain as a payments app.
//
//   Home      balance, Send / Receive / Pay shop, requests from other apps,
//             and what happened (sent, received, in progress)
//   Send      amount, who, an optional note, and one switch: send privately
//   Receive   your name and payment link — that's all anyone needs to pay you
//   More      backup, private balance, network fees, and the station team's
//             tools: the shop counter and the organizer
//
// Everything slow happens in fieldstore_core on its own thread; this file polls
// its `state()` once a second and queues actions.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root
    implicitWidth: 960
    implicitHeight: 760

    readonly property string coreId: "fieldstore_core"

    property var st: ({})
    property string page: "home"          // home, send, receive, more, counter, organizer
    property string lastError: ""
    property string copied: ""
    property int watchJob: -1             // the send we're waiting to celebrate
    property var receipt: null
    property string recoveryWords: ""

    // Send form
    property string sendTo: ""            // "shop", an address, or a fieldpay: link
    property string sendToName: ""
    property bool sendPrivate: false

    readonly property bool ready: st.phase === "ready"
    readonly property var me: st.me || ({})
    readonly property var station: st.station || ({})
    readonly property var counter: st.counter || ({})
    readonly property var org: st.organizer || ({})
    readonly property var net: st.network || ({})
    readonly property var activity: st.activity || []
    readonly property var contacts: st.contacts || []
    readonly property var pendingRequests: (st.requests || []).filter(function (q) {
        return q.status === "pending";
    })
    readonly property bool joined: station.joined === true
    readonly property string sym: station.name && station.name.length ? station.name : "tokens"

    // ── Logos bridge ─────────────────────────────────────────────────
    //   logos.callModule(id, method, [])            — synchronous, no-arg only
    //   logos.callModuleAsync(id, method, args, cb) — anything with arguments
    function call(method) {
        if (typeof logos === "undefined" || !logos.callModule)
            return null;
        return logos.callModule(coreId, method, []);
    }
    function callArgs(method, args, cb) {
        if (typeof logos === "undefined" || !logos.callModuleAsync) {
            lastError = "The Logos bridge is unavailable.";
            if (cb)
                cb(null);
            return;
        }
        logos.callModuleAsync(coreId, method, args, function (raw) {
            var r = unwrap(raw, null);
            if (r && r.ok === false)
                lastError = r.error || "Something went wrong.";
            refresh();
            if (cb)
                cb(r);
        });
    }
    // The bridge JSON-encodes whatever the module returned, so a method whose
    // own return value is already JSON arrives double-encoded. Keep parsing
    // while the result is still a string; stop when a parse fails.
    function unwrap(raw, def) {
        if (raw === null || raw === undefined)
            return def;
        var v = raw;
        for (var i = 0; i < 3 && typeof v === "string"; ++i) {
            try {
                v = JSON.parse(v);
            } catch (e) {
                return (i === 0) ? def : v;
            }
        }
        return v;
    }
    function refresh() {
        var s = unwrap(call("state"), null);
        if (s && typeof s === "object") {
            st = s;
            watchSend();
        }
    }
    function watchSend() {
        if (watchJob < 0)
            return;
        for (var i = 0; i < activity.length; ++i) {
            var a = activity[i];
            if (a.job !== watchJob)
                continue;
            if (a.status === "done") {
                receipt = a;
                watchJob = -1;
            } else if (a.status === "failed") {
                lastError = a.error || "The payment didn't go through.";
                watchJob = -1;
            }
        }
    }

    // ── Actions ──────────────────────────────────────────────────────
    function openSend(to, name) {
        sendTo = to || "";
        sendToName = name || "";
        amountInput.text = "";
        noteInput.text = "";
        pasteInput.text = "";
        sendPrivate = false;
        lastError = "";
        page = "send";
        amountInput.forceActiveFocus();
    }
    function doSend() {
        lastError = "";
        callArgs("send", [sendTo, amountInput.text.trim(), sendPrivate ? "private" : "public", noteInput.text.trim()], function (r) {
            if (r && r.ok) {
                watchJob = r.job;
                page = "home";
            }
        });
    }

    // ── Helpers ──────────────────────────────────────────────────────
    function num(s) {
        var n = Number(s || 0);
        return isNaN(n) ? 0 : n;
    }
    function fmt(s) {
        return num(s).toLocaleString(Qt.locale("en_US"), "f", 0);
    }
    function shortAddr(a) {
        a = a || "";
        return a.length > 12 ? a.substring(0, 4) + "…" + a.substring(a.length - 4) : a;
    }
    function shortTx(t) {
        return (t || "").substring(0, 8).toUpperCase();
    }
    function clockOf(ms) {
        if (!ms)
            return "";
        var d = new Date(ms);
        function p(n) {
            return (n < 10 ? "0" : "") + n;
        }
        return p(d.getHours()) + ":" + p(d.getMinutes());
    }
    function whenOf(ms) {
        if (!ms)
            return "";
        var s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
        if (s < 45)
            return "just now";
        if (s < 3600)
            return Math.max(1, Math.round(s / 60)) + " min ago";
        var d = new Date(ms), n = new Date();
        return (d.toDateString() === n.toDateString() ? "" : d.toLocaleDateString(Qt.locale("en_US"), "d MMM") + " ") + clockOf(ms);
    }
    function initials(name) {
        var n = (name || "").trim();
        if (!n.length)
            return "?";
        var parts = n.split(/\s+/);
        return (parts.length > 1 ? parts[0].charAt(0) + parts[1].charAt(0) : n.substring(0, 1)).toUpperCase();
    }
    function hueFor(seed) {
        var h = 0;
        var s = seed || "";
        for (var i = 0; i < s.length; i++)
            h = (h * 31 + s.charCodeAt(i)) >>> 0;
        return (h % 360) / 360;
    }
    function busyText(status) {
        switch (status) {
        case "queued":
            return "Waiting to send…";
        case "working":
            return "Sending…";
        case "preparing":
            return "Getting private funds ready…";
        case "proving":
            return "Making it private · 1–2 min";
        case "confirming":
            return "Confirming…";
        }
        return "";
    }
    function inProgress(s) {
        return s !== "done" && s !== "failed";
    }
    function linkName(t) {
        t = (t || "").trim();
        if (t.indexOf("fieldpay:") !== 0)
            return "";
        var rest = t.substring(9);
        var c = rest.indexOf(":");
        return c >= 0 ? rest.substring(c + 1) : "";
    }
    function looksLikeAddress(t) {
        t = (t || "").trim();
        return t.indexOf("fieldpay:") === 0 || /^[1-9A-HJ-NP-Za-km-z]{32,44}$/.test(t.replace(/^Public\//, ""));
    }
    function jobBusy(kind) {
        return (st.jobs || []).some(function (j) {
            return j.kind === kind && inProgress(j.status);
        });
    }

    TextEdit {
        id: clip
        visible: false
    }
    function copy(t, key) {
        clip.text = t;
        clip.selectAll();
        clip.copy();
        copied = key;
        copyTimer.restart();
    }
    Timer {
        id: copyTimer
        interval: 1600
        onTriggered: root.copied = ""
    }
    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Gradient {
        id: accentGrad
        GradientStop {
            position: 0.0
            color: "#F28E6B"
        }
        GradientStop {
            position: 1.0
            color: "#E1613A"
        }
    }

    // ── Building blocks ──────────────────────────────────────────────
    component Card: Rectangle {
        id: card
        default property alias content: inner.data
        property int pad: Theme.spacing.xlarge
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + pad * 2
        color: Theme.palette.backgroundElevated
        radius: Theme.spacing.radiusXlarge
        border.color: Theme.palette.borderSecondary
        border.width: 1
        ColumnLayout {
            id: inner
            anchors.fill: parent
            anchors.margins: card.pad
            spacing: Theme.spacing.medium
        }
    }

    component Caption: LogosText {
        Layout.fillWidth: true
        color: Theme.palette.textTertiary
        font.pixelSize: Theme.typography.secondaryText
        wrapMode: Text.WordWrap
        lineHeight: 1.15
    }

    component Heading: LogosText {
        Layout.fillWidth: true
        color: Theme.palette.text
        font.pixelSize: Theme.typography.subtitleText
        font.weight: Theme.typography.weightBold
        wrapMode: Text.WordWrap
    }

    component Action: Rectangle {
        id: act
        property string text: ""
        property bool primary: true
        property bool busy: false
        property bool danger: false
        signal clicked
        implicitHeight: 48
        implicitWidth: Math.max(120, lbl.implicitWidth + 44)
        radius: Theme.spacing.radiusPill
        opacity: enabled ? 1 : 0.4
        gradient: primary && enabled ? accentGrad : null
        color: primary ? Theme.palette.primary : (ma.containsMouse ? Theme.palette.backgroundMuted : Theme.palette.backgroundSecondary)
        border.color: primary ? "transparent" : (danger ? Theme.palette.error : Theme.palette.border)
        border.width: primary ? 0 : 1
        RowLayout {
            anchors.centerIn: parent
            spacing: Theme.spacing.small
            LogosSpinner {
                visible: act.busy
                running: act.busy
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                ringColor: act.primary ? "#ffffff" : Theme.palette.text
                thickness: 2
                dotSize: 4
            }
            LogosText {
                id: lbl
                text: act.text
                color: act.primary ? "#ffffff" : (act.danger ? Theme.palette.error : Theme.palette.text)
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightBold
            }
        }
        MouseArea {
            id: ma
            anchors.fill: parent
            hoverEnabled: true
            enabled: act.enabled && !act.busy
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: act.clicked()
        }
    }

    component LinkText: LogosText {
        id: lt
        signal clicked
        color: Theme.palette.primary
        font.pixelSize: Theme.typography.secondaryText
        font.weight: Theme.typography.weightMedium
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: lt.clicked()
        }
    }

    component Field: LogosTextField {
        Layout.fillWidth: true
        implicitHeight: 46
    }

    component Avatar: Rectangle {
        id: av
        property string name: ""
        property string glyph: ""
        property color tint: Qt.hsla(root.hueFor(name), 0.45, 0.42, 1)
        implicitWidth: 40
        implicitHeight: 40
        radius: width / 2
        color: tint
        LogosText {
            anchors.centerIn: parent
            text: av.glyph.length ? av.glyph : root.initials(av.name)
            color: "#ffffff"
            font.pixelSize: av.width > 30 ? (av.glyph.length ? 18 : 15) : 12
            font.weight: Theme.typography.weightBold
        }
    }

    component Tile: Rectangle {
        id: tile
        property string glyph: ""
        property string text: ""
        property bool primary: false
        signal clicked
        Layout.fillWidth: true
        implicitHeight: 84
        radius: Theme.spacing.radiusXlarge
        gradient: primary ? accentGrad : null
        color: tileMa.containsMouse ? Theme.palette.backgroundMuted : Theme.palette.backgroundElevated
        border.color: primary ? "transparent" : Theme.palette.borderSecondary
        border.width: primary ? 0 : 1
        ColumnLayout {
            anchors.centerIn: parent
            spacing: 4
            LogosText {
                Layout.alignment: Qt.AlignHCenter
                text: tile.glyph
                color: tile.primary ? "#ffffff" : Theme.palette.primary
                font.pixelSize: 22
                font.weight: Theme.typography.weightBold
            }
            LogosText {
                Layout.alignment: Qt.AlignHCenter
                text: tile.text
                color: tile.primary ? "#ffffff" : Theme.palette.text
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightBold
            }
        }
        MouseArea {
            id: tileMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: tile.clicked()
        }
    }

    component CopyBox: Rectangle {
        id: cb
        property string label: ""
        property string value: ""
        property string key: ""
        Layout.fillWidth: true
        implicitHeight: cbRow.implicitHeight + 2 * Theme.spacing.medium
        radius: Theme.spacing.radiusLarge
        color: Theme.palette.backgroundInset
        border.color: Theme.palette.borderHairline
        RowLayout {
            id: cbRow
            anchors.fill: parent
            anchors.margins: Theme.spacing.medium
            spacing: Theme.spacing.medium
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                LogosText {
                    visible: cb.label.length > 0
                    text: cb.label
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
                LogosText {
                    Layout.fillWidth: true
                    text: cb.value
                    wrapMode: Text.WrapAnywhere
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    color: Theme.palette.text
                    font.family: "Menlo"
                    font.pixelSize: Theme.typography.secondaryText
                }
            }
            Action {
                primary: false
                implicitHeight: 36
                implicitWidth: 92
                text: root.copied === cb.key ? "Copied ✓" : "Copy"
                enabled: cb.value.length > 0
                onClicked: root.copy(cb.value, cb.key)
            }
        }
    }

    component Section: ColumnLayout {
        id: sec
        property string title: ""
        Layout.fillWidth: true
        spacing: Theme.spacing.small
        LogosText {
            text: sec.title
            color: Theme.palette.textTertiary
            font.pixelSize: Theme.typography.secondaryText
            font.weight: Theme.typography.weightBold
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 0.6
            Layout.leftMargin: 4
        }
    }

    component MenuRow: Rectangle {
        id: mr
        property string title: ""
        property string detail: ""
        signal clicked
        Layout.fillWidth: true
        implicitHeight: 62
        radius: Theme.spacing.radiusLarge
        color: mrMa.containsMouse ? Theme.palette.backgroundMuted : Theme.palette.backgroundElevated
        border.color: Theme.palette.borderSecondary
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacing.large
            anchors.rightMargin: Theme.spacing.large
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                LogosText {
                    text: mr.title
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.primaryText
                    font.weight: Theme.typography.weightMedium
                }
                LogosText {
                    visible: mr.detail.length > 0
                    Layout.fillWidth: true
                    text: mr.detail
                    elide: Text.ElideRight
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
            }
            LogosText {
                text: "›"
                color: Theme.palette.textTertiary
                font.pixelSize: 22
            }
        }
        MouseArea {
            id: mrMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: mr.clicked()
        }
    }

    // ── Page ─────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Top bar
        Item {
            Layout.fillWidth: true
            implicitHeight: 64
            RowLayout {
                width: Math.min(parent.width - 2 * Theme.spacing.xlarge, 560)
                anchors.centerIn: parent
                spacing: Theme.spacing.medium
                Rectangle {
                    visible: root.page !== "home"
                    implicitWidth: 36
                    implicitHeight: 36
                    radius: 18
                    color: backMa.containsMouse ? Theme.palette.backgroundMuted : Theme.palette.backgroundSecondary
                    border.color: Theme.palette.borderSecondary
                    LogosText {
                        anchors.centerIn: parent
                        text: "←"
                        color: Theme.palette.text
                        font.pixelSize: 18
                    }
                    MouseArea {
                        id: backMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.lastError = "";
                            root.page = (root.page === "counter" || root.page === "organizer") && root.joined ? "more" : "home";
                        }
                    }
                }
                Rectangle {
                    visible: root.page === "home"
                    implicitWidth: 32
                    implicitHeight: 32
                    radius: 9
                    gradient: accentGrad
                    LogosText {
                        anchors.centerIn: parent
                        text: "F"
                        color: "#ffffff"
                        font.pixelSize: 16
                        font.weight: Theme.typography.weightBold
                    }
                }
                LogosText {
                    Layout.fillWidth: true
                    text: ({
                            home: "Field Pay",
                            send: "Send",
                            receive: "Receive",
                            more: "More",
                            counter: "Shop counter",
                            organizer: "Organizer"
                        })[root.page]
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.subtitleText + 2
                    font.weight: Theme.typography.weightBold
                }
                Rectangle {
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    color: !root.ready ? Theme.palette.warning : root.net.online ? Theme.palette.success : Theme.palette.error
                }
                LogosText {
                    text: !root.ready ? (root.st.phase === "error" ? "Wallet error" : "Starting") : root.net.online ? "Online" : "Offline"
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
                Rectangle {
                    visible: root.page === "home" && root.ready
                    implicitWidth: 36
                    implicitHeight: 36
                    radius: 18
                    color: moreMa.containsMouse ? Theme.palette.backgroundMuted : Theme.palette.backgroundSecondary
                    border.color: Theme.palette.borderSecondary
                    LogosText {
                        anchors.centerIn: parent
                        text: "⋯"
                        color: Theme.palette.text
                        font.pixelSize: 18
                    }
                    MouseArea {
                        id: moreMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.page = "more"
                    }
                }
            }
        }

        // Error strip
        Rectangle {
            Layout.fillWidth: true
            visible: root.lastError.length > 0 || root.st.phase === "error"
            implicitHeight: errRow.implicitHeight + 2 * Theme.spacing.medium
            color: Theme.colors.getColor(Theme.palette.error, 0.14)
            RowLayout {
                id: errRow
                width: Math.min(parent.width - 2 * Theme.spacing.xlarge, 560)
                anchors.centerIn: parent
                LogosText {
                    Layout.fillWidth: true
                    text: root.st.phase === "error" ? (root.st.error || "The wallet failed to start.") : root.lastError
                    color: Theme.palette.text
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.typography.secondaryText
                }
                LinkText {
                    visible: root.st.phase !== "error"
                    text: "Dismiss"
                    onClicked: root.lastError = ""
                }
            }
        }

        ScrollView {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: Math.min(scroller.availableWidth - 2 * Theme.spacing.xlarge, 560)
                x: (scroller.availableWidth - width) / 2
                spacing: Theme.spacing.large

                Item {
                    implicitHeight: Theme.spacing.small
                }

                // Starting
                Card {
                    visible: !root.ready && root.st.phase !== "error"
                    RowLayout {
                        spacing: Theme.spacing.medium
                        LogosSpinner {
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                        }
                        LogosText {
                            text: "Opening your wallet…"
                            color: Theme.palette.textSecondary
                        }
                    }
                }

                // ═══ WELCOME (not in a station yet) ══════════════════
                ColumnLayout {
                    visible: root.ready && !root.joined && root.page === "home"
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large
                    Card {
                        pad: 32
                        LogosText {
                            text: "👋"
                            font.pixelSize: 40
                        }
                        LogosText {
                            Layout.fillWidth: true
                            text: "Welcome to Field Pay"
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.panelTitleText
                            font.weight: Theme.typography.weightBold
                        }
                        Caption {
                            text: "The money for this residency. Paste the station code the organizer shared to get started."
                            font.pixelSize: Theme.typography.primaryText
                        }
                        Field {
                            id: codeField
                            placeholderText: "Station code (fieldstore1:…)"
                        }
                        Action {
                            Layout.fillWidth: true
                            text: "Join"
                            enabled: codeField.text.trim().length > 20
                            busy: root.jobBusy("join")
                            onClicked: {
                                root.lastError = "";
                                root.callArgs("joinStation", [codeField.text], function (r) {
                                    if (r && r.ok)
                                        codeField.text = "";
                                });
                            }
                        }
                    }
                    LinkText {
                        Layout.alignment: Qt.AlignHCenter
                        text: "Starting a new station? Organizer tools →"
                        onClicked: root.page = "organizer"
                    }
                }

                // ═══ HOME ════════════════════════════════════════════
                ColumnLayout {
                    visible: root.ready && root.joined && root.page === "home"
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    // Receipt for the payment you just made
                    Rectangle {
                        Layout.fillWidth: true
                        visible: root.receipt !== null
                        implicitHeight: rcpt.implicitHeight + 2 * Theme.spacing.xlarge
                        radius: Theme.spacing.radiusXlarge
                        color: Theme.colors.getColor(Theme.palette.success, 0.12)
                        border.color: Theme.palette.success
                        border.width: 2
                        ColumnLayout {
                            id: rcpt
                            anchors.fill: parent
                            anchors.margins: Theme.spacing.xlarge
                            spacing: Theme.spacing.small
                            LogosText {
                                text: "✓ Sent"
                                color: Theme.palette.success
                                font.pixelSize: Theme.typography.subtitleText
                                font.weight: Theme.typography.weightBold
                            }
                            LogosText {
                                text: root.receipt ? root.fmt(root.receipt.amount) + " " + root.sym : ""
                                color: Theme.palette.text
                                font.pixelSize: 44
                                font.weight: Theme.typography.weightBold
                            }
                            LogosText {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: root.receipt ? "to " + (root.receipt.who || root.shortAddr(root.receipt.address)) + (root.receipt.private ? " · 🔒 private" : "") + " · receipt " + root.shortTx(root.receipt.tx) : ""
                                color: Theme.palette.textSecondary
                                font.pixelSize: Theme.typography.primaryText
                            }
                            Caption {
                                visible: root.receipt !== null && root.receipt.who === "Shop"
                                text: "It's already on the shop counter's screen."
                            }
                            Action {
                                primary: false
                                text: "Done"
                                onClicked: root.receipt = null
                            }
                        }
                    }

                    // Requests from other apps
                    Repeater {
                        model: root.pendingRequests
                        delegate: Rectangle {
                            id: reqCard
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: reqCol.implicitHeight + 2 * Theme.spacing.large
                            radius: Theme.spacing.radiusXlarge
                            color: Theme.palette.overlayOrange
                            border.color: Theme.palette.primary
                            ColumnLayout {
                                id: reqCol
                                anchors.fill: parent
                                anchors.margins: Theme.spacing.large
                                spacing: Theme.spacing.small
                                LogosText {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: reqCard.modelData.app + " asks you to pay " + root.fmt(reqCard.modelData.amount) + " " + root.sym
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightBold
                                }
                                Caption {
                                    text: (reqCard.modelData.memo ? reqCard.modelData.memo + " · " : "") + "to " + (reqCard.modelData.who || root.shortAddr(reqCard.modelData.to))
                                    color: Theme.palette.textSecondary
                                }
                                RowLayout {
                                    spacing: Theme.spacing.small
                                    Action {
                                        text: "Pay"
                                        implicitHeight: 40
                                        onClicked: root.callArgs("approveRequest", [reqCard.modelData.id, "public"], function (r) {
                                            if (r && r.ok)
                                                root.watchJob = r.job;
                                        })
                                    }
                                    Action {
                                        primary: false
                                        text: "Pay privately"
                                        implicitHeight: 40
                                        onClicked: root.callArgs("approveRequest", [reqCard.modelData.id, "private"], function (r) {
                                            if (r && r.ok)
                                                root.watchJob = r.job;
                                        })
                                    }
                                    Item {
                                        Layout.fillWidth: true
                                    }
                                    LinkText {
                                        text: "Decline"
                                        color: Theme.palette.textSecondary
                                        onClicked: root.callArgs("declineRequest", [reqCard.modelData.id])
                                    }
                                }
                            }
                        }
                    }

                    // Balance
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.spacing.medium
                        spacing: 2
                        LogosText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: "Balance"
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.primaryText
                        }
                        Item {
                            Layout.fillWidth: true
                            implicitHeight: balRow.implicitHeight
                        Row {
                            id: balRow
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: Theme.spacing.small
                            LogosText {
                                text: root.fmt(root.me.total)
                                color: Theme.palette.text
                                font.pixelSize: 60
                                font.weight: Theme.typography.weightBold
                            }
                            LogosText {
                                anchors.baseline: parent.children[0].baseline
                                text: root.sym
                                color: Theme.palette.textSecondary
                                font.pixelSize: Theme.typography.panelTitleText
                                font.weight: Theme.typography.weightMedium
                            }
                        }
                        }
                        LogosText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            visible: root.num(root.me.privateTokens) > 0
                            text: "🔒 " + root.fmt(root.me.privateTokens) + " of it is private"
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.spacing.small
                        spacing: Theme.spacing.medium
                        Tile {
                            primary: true
                            glyph: "↑"
                            text: "Send"
                            onClicked: root.openSend("", "")
                        }
                        Tile {
                            glyph: "↓"
                            text: "Receive"
                            onClicked: root.page = "receive"
                        }
                        Tile {
                            glyph: "◎"
                            text: "Pay shop"
                            onClicked: root.openSend("shop", "Shop")
                        }
                    }

                    // Backup nudge
                    Rectangle {
                        Layout.fillWidth: true
                        visible: !!(root.st.recovery && root.st.recovery.available && !root.st.recovery.acknowledged)
                        implicitHeight: nudge.implicitHeight + 2 * Theme.spacing.medium
                        radius: Theme.spacing.radiusLarge
                        color: Theme.colors.getColor(Theme.palette.warning, 0.10)
                        border.color: Theme.colors.getColor(Theme.palette.warning, 0.5)
                        RowLayout {
                            id: nudge
                            anchors.fill: parent
                            anchors.margins: Theme.spacing.medium
                            LogosText {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: "Back up your wallet so your " + root.sym + " survive a lost laptop."
                                color: Theme.palette.text
                                font.pixelSize: Theme.typography.secondaryText
                            }
                            LinkText {
                                text: "Back up"
                                onClicked: root.page = "more"
                            }
                        }
                    }

                    // Activity
                    LogosText {
                        Layout.topMargin: Theme.spacing.small
                        text: "Activity"
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                    }
                    Caption {
                        visible: root.activity.length === 0
                        text: "Nothing yet. Payments you send and receive show up here."
                    }
                    Repeater {
                        model: root.activity
                        delegate: RowLayout {
                            id: row
                            required property var modelData
                            readonly property bool out: modelData.dir === "out"
                            readonly property bool busy: root.inProgress(modelData.status)
                            readonly property bool failed: modelData.status === "failed"
                            readonly property string who: out ? (modelData.who || root.shortAddr(modelData.address)) : (modelData.who || "")
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            Avatar {
                                name: row.who
                                glyph: !row.out ? "↓" : row.who === "Shop" ? "◎" : ""
                                tint: !row.out ? Theme.colors.getColor(Theme.palette.success, 0.85) : row.who === "Shop" ? Theme.palette.primary : Qt.hsla(root.hueFor(row.who), 0.45, 0.42, 1)
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                LogosText {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: row.out ? row.who : (row.who.length ? "From " + row.who : "Received")
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightMedium
                                }
                                LogosText {
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    text: row.failed ? "Didn't go through — " + row.modelData.error : row.busy ? root.busyText(row.modelData.status) : [row.modelData.note, root.whenOf(row.modelData.at), row.modelData.private ? "🔒 private" : ""].filter(function (x) {
                                        return x && x.length;
                                    }).join(" · ")
                                    color: row.failed ? Theme.palette.error : row.busy ? Theme.palette.warning : Theme.palette.textTertiary
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                            }
                            LogosSpinner {
                                visible: row.busy
                                running: visible
                                Layout.preferredWidth: 14
                                Layout.preferredHeight: 14
                                thickness: 2
                                dotSize: 4
                            }
                            LogosText {
                                text: (row.out ? "−" : "+") + root.fmt(row.modelData.amount)
                                color: row.failed ? Theme.palette.textMuted : row.out ? Theme.palette.text : Theme.palette.success
                                font.strikeout: row.failed
                                font.pixelSize: Theme.typography.subtitleText
                                font.weight: Theme.typography.weightBold
                            }
                        }
                    }
                }

                // ═══ SEND ════════════════════════════════════════════
                ColumnLayout {
                    visible: root.ready && root.page === "send"
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    // Amount
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.spacing.medium
                        spacing: 0
                        Item {
                            Layout.fillWidth: true
                            implicitHeight: amtRow.implicitHeight
                        Row {
                            id: amtRow
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: Theme.spacing.small
                            TextInput {
                                id: amountInput
                                width: Math.max(48, contentWidth + 6)
                                color: Theme.palette.text
                                font.pixelSize: 60
                                font.weight: Theme.typography.weightBold
                                horizontalAlignment: TextInput.AlignHCenter
                                validator: IntValidator {
                                    bottom: 1
                                    top: 999999999
                                }
                                selectByMouse: true
                                LogosText {
                                    anchors.centerIn: parent
                                    visible: amountInput.text.length === 0
                                    text: "0"
                                    color: Theme.palette.textMuted
                                    font.pixelSize: 60
                                    font.weight: Theme.typography.weightBold
                                }
                            }
                            LogosText {
                                anchors.baseline: amountInput.baseline
                                text: root.sym
                                color: Theme.palette.textSecondary
                                font.pixelSize: Theme.typography.panelTitleText
                            }
                        }
                        }
                        LogosText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            readonly property real avail: root.sendPrivate ? root.num(root.me.total) : root.num(root.me.publicTokens)
                            text: root.num(amountInput.text) > avail ? "More than you can send (" + root.fmt(avail) + ")" : "You have " + root.fmt(root.me.total)
                            color: root.num(amountInput.text) > avail ? Theme.palette.error : Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                        }
                    }

                    // To
                    Card {
                        LogosText {
                            text: "To"
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                            font.weight: Theme.typography.weightBold
                        }
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small
                            Repeater {
                                model: root.contacts
                                delegate: Rectangle {
                                    id: chip
                                    required property var modelData
                                    readonly property bool chosen: root.sendTo === (modelData.shop ? "shop" : modelData.address)
                                    width: chipRow.implicitWidth + 24
                                    height: 40
                                    radius: 20
                                    color: chosen ? Theme.palette.overlayOrange : Theme.palette.backgroundInset
                                    border.color: chosen ? Theme.palette.primary : Theme.palette.borderSecondary
                                    border.width: chosen ? 2 : 1
                                    RowLayout {
                                        id: chipRow
                                        anchors.centerIn: parent
                                        spacing: 8
                                        Avatar {
                                            implicitWidth: 24
                                            implicitHeight: 24
                                            name: chip.modelData.name
                                            glyph: chip.modelData.shop ? "◎" : ""
                                            tint: chip.modelData.shop ? Theme.palette.primary : Qt.hsla(root.hueFor(chip.modelData.name), 0.45, 0.42, 1)
                                        }
                                        LogosText {
                                            text: chip.modelData.name
                                            color: Theme.palette.text
                                            font.pixelSize: Theme.typography.primaryText
                                        }
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            root.sendTo = chip.modelData.shop ? "shop" : chip.modelData.address;
                                            root.sendToName = chip.modelData.name;
                                            pasteInput.text = "";
                                        }
                                    }
                                }
                            }
                        }
                        Field {
                            id: pasteInput
                            placeholderText: "…or paste someone's payment link"
                            onTextChanged: {
                                var t = text.trim();
                                if (root.looksLikeAddress(t)) {
                                    root.sendTo = t;
                                    root.sendToName = root.linkName(t);
                                } else if (root.sendTo.length && root.sendTo !== "shop" && root.contacts.every(function (c) {
                                        return c.address !== root.sendTo;
                                    })) {
                                    root.sendTo = "";
                                    root.sendToName = "";
                                }
                            }
                        }
                        Caption {
                            visible: pasteInput.text.trim().length > 0 && !root.looksLikeAddress(pasteInput.text)
                            text: "That doesn't look like a payment link — ask them for the one on their Receive screen."
                            color: Theme.palette.warning
                        }
                    }

                    Field {
                        id: noteInput
                        placeholderText: "What's it for? (only you see this)"
                    }

                    // Privacy
                    Card {
                        pad: Theme.spacing.large
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                LogosText {
                                    text: "🔒 Send privately"
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightBold
                                }
                                Caption {
                                    text: root.sendPrivate ? "Nobody can see this came from you. Takes 2–3 minutes while your laptop prepares it. No fee." : "Arrives in about a minute. Like a card payment, the ledger shows your address and the amount."
                                }
                            }
                            LogosSwitch {
                                checked: root.sendPrivate
                                onToggled: root.sendPrivate = checked
                            }
                        }
                    }

                    Action {
                        Layout.fillWidth: true
                        implicitHeight: 54
                        readonly property real avail: root.sendPrivate ? root.num(root.me.total) : root.num(root.me.publicTokens)
                        text: root.sendTo.length === 0 ? "Choose who to pay" : root.num(amountInput.text) <= 0 ? "Enter an amount" : "Send " + root.fmt(amountInput.text) + " " + root.sym + " to " + (root.sendToName || root.shortAddr(root.sendTo.replace("fieldpay:", "")))
                        enabled: root.sendTo.length > 0 && root.num(amountInput.text) > 0 && root.num(amountInput.text) <= avail
                        onClicked: root.doSend()
                    }
                    Caption {
                        visible: !root.sendPrivate && root.num(root.me.privateTokens) > 0 && root.num(amountInput.text) > root.num(root.me.publicTokens) && root.num(amountInput.text) <= root.num(root.me.total)
                        text: "Some of your balance is private, so this one needs to go privately."
                        color: Theme.palette.warning
                    }
                }

                // ═══ RECEIVE ═════════════════════════════════════════
                ColumnLayout {
                    visible: root.ready && root.page === "receive"
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    Card {
                        Heading {
                            text: "Your payment link"
                        }
                        Caption {
                            text: "Anyone at the station pastes this into Send to pay you. The organizer uses it to send your welcome " + root.sym + "."
                        }
                        CopyBox {
                            value: root.me.link || ""
                            key: "link"
                        }
                        Caption {
                            text: "Payments to you arrive in about a minute and show up in your activity."
                        }
                    }

                    Card {
                        Heading {
                            text: "Your name"
                        }
                        Caption {
                            text: "Added to your link, so people paying you see who they're paying."
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small
                            Field {
                                id: nameInput
                                text: root.me.name || ""
                                placeholderText: "e.g. Alex"
                            }
                            Action {
                                primary: false
                                text: "Save"
                                enabled: nameInput.text.trim() !== (root.me.name || "")
                                onClicked: root.callArgs("setName", [nameInput.text.trim()])
                            }
                        }
                    }
                }

                // ═══ MORE ════════════════════════════════════════════
                ColumnLayout {
                    visible: root.ready && root.page === "more"
                    Layout.fillWidth: true
                    spacing: Theme.spacing.xlarge

                    Section {
                        title: "Your wallet"
                        Card {
                            Heading {
                                text: root.st.recovery && root.st.recovery.acknowledged ? "Recovery words" : "Back up your wallet"
                            }
                            Caption {
                                text: "These 24 words restore your " + root.sym + " on a new laptop. Write them down and keep them somewhere only you can reach."
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                visible: root.recoveryWords.length > 0
                                implicitHeight: words.implicitHeight + 2 * Theme.spacing.medium
                                radius: Theme.spacing.radiusMedium
                                color: Theme.palette.backgroundInset
                                LogosText {
                                    id: words
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacing.medium
                                    text: root.recoveryWords
                                    wrapMode: Text.WordWrap
                                    font.family: "Menlo"
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.primaryText
                                    lineHeight: 1.3
                                }
                            }
                            RowLayout {
                                spacing: Theme.spacing.small
                                Action {
                                    primary: !root.recoveryWords.length && !(root.st.recovery && root.st.recovery.acknowledged)
                                    text: root.recoveryWords.length ? "Hide" : "Show words"
                                    onClicked: {
                                        if (root.recoveryWords.length) {
                                            root.recoveryWords = "";
                                            return;
                                        }
                                        root.callArgs("recoveryPhrase", [], function (r) {
                                            if (r && r.ok)
                                                root.recoveryWords = r.words;
                                        });
                                    }
                                }
                                Action {
                                    visible: root.recoveryWords.length > 0 && !(root.st.recovery && root.st.recovery.acknowledged)
                                    text: "I've written them down"
                                    onClicked: {
                                        root.callArgs("acknowledgeRecovery", []);
                                        root.recoveryWords = "";
                                    }
                                }
                            }
                        }

                        Card {
                            visible: root.joined
                            Heading {
                                text: "Private balance"
                            }
                            Caption {
                                text: "Private " + root.sym + " can be spent without anyone seeing it came from you. Sending privately fills this up for you — these buttons are only for moving a lot at once. Each move takes a minute or two."
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.xlarge
                                ColumnLayout {
                                    spacing: 0
                                    LogosText {
                                        text: "Public"
                                        color: Theme.palette.textTertiary
                                        font.pixelSize: Theme.typography.secondaryText
                                    }
                                    LogosText {
                                        text: root.fmt(root.me.publicTokens)
                                        color: Theme.palette.text
                                        font.pixelSize: Theme.typography.panelTitleText
                                        font.weight: Theme.typography.weightBold
                                    }
                                }
                                ColumnLayout {
                                    spacing: 0
                                    LogosText {
                                        text: "🔒 Private"
                                        color: Theme.palette.textTertiary
                                        font.pixelSize: Theme.typography.secondaryText
                                    }
                                    LogosText {
                                        text: root.fmt(root.me.privateTokens)
                                        color: Theme.palette.info
                                        font.pixelSize: Theme.typography.panelTitleText
                                        font.weight: Theme.typography.weightBold
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                Field {
                                    id: moveAmount
                                    Layout.preferredWidth: 120
                                    Layout.fillWidth: false
                                    placeholderText: "Amount"
                                    textInput.validator: IntValidator {
                                        bottom: 1
                                    }
                                }
                                Action {
                                    primary: false
                                    text: "Make private"
                                    busy: root.jobBusy("shield")
                                    enabled: root.num(moveAmount.text) > 0 && root.num(moveAmount.text) <= root.num(root.me.publicTokens)
                                    onClicked: root.callArgs("moveToPrivate", [moveAmount.text.trim()], function (r) {
                                        if (r && r.ok)
                                            moveAmount.text = "";
                                    })
                                }
                                Action {
                                    primary: false
                                    text: "Make public"
                                    busy: root.jobBusy("unshield")
                                    enabled: root.num(moveAmount.text) > 0 && root.num(moveAmount.text) <= root.num(root.me.privateTokens)
                                    onClicked: root.callArgs("moveToPublic", [moveAmount.text.trim()], function (r) {
                                        if (r && r.ok)
                                            moveAmount.text = "";
                                    })
                                }
                            }
                        }

                        Card {
                            Heading {
                                text: "Network fees"
                            }
                            Caption {
                                text: "Ordinary payments cost a tiny fee in LGO, the testnet's own token. Field Pay tops it up for you from the testnet faucet. Private payments are free."
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                LogosText {
                                    Layout.fillWidth: true
                                    text: (root.me.gas || "0") + " LGO"
                                    color: root.me.hasGas ? Theme.palette.text : Theme.palette.warning
                                    font.pixelSize: Theme.typography.subtitleText
                                    font.weight: Theme.typography.weightBold
                                }
                                Action {
                                    primary: false
                                    text: "Top up now"
                                    busy: root.jobBusy("gas")
                                    onClicked: root.callArgs("getGas", [])
                                }
                            }
                        }
                    }

                    Section {
                        title: "Station team"
                        MenuRow {
                            title: "Shop counter"
                            detail: "Live payments into the shop — keep it open at the till"
                            onClicked: root.page = "counter"
                        }
                        MenuRow {
                            title: "Organizer"
                            detail: root.org.active ? "Welcome residents, mint, share the station code" : "Start a station and its token"
                            onClicked: root.page = "organizer"
                        }
                    }

                    Section {
                        visible: root.joined
                        title: "Station"
                        Card {
                            pad: Theme.spacing.large
                            Caption {
                                text: "You're in " + root.sym + " · " + root.fmt(root.station.supply) + " in circulation · block " + (root.net.block || 0)
                            }
                            CopyBox {
                                label: "Station code — share it with new arrivals"
                                value: root.station.code || ""
                                key: "code"
                            }
                            LinkText {
                                visible: !root.org.active
                                text: "Leave this station"
                                color: Theme.palette.error
                                onClicked: root.callArgs("leaveStation", [], function (r) {
                                    if (r && r.ok)
                                        root.page = "home";
                                })
                            }
                        }
                    }
                }

                // ═══ COUNTER ═════════════════════════════════════════
                ColumnLayout {
                    visible: root.ready && root.page === "counter"
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    Caption {
                        visible: !root.joined
                        text: "Join a station first — the counter watches its shop."
                    }
                    ColumnLayout {
                        visible: root.joined
                        Layout.fillWidth: true
                        spacing: 2
                        LogosText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: "In the till"
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.primaryText
                        }
                        LogosText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: root.fmt(root.counter.tillBalance) + " " + root.sym
                            color: Theme.palette.text
                            font.pixelSize: 52
                            font.weight: Theme.typography.weightBold
                        }
                        Caption {
                            horizontalAlignment: Text.AlignHCenter
                            text: "Hand over the goods when the amount appears here — not from the customer's screen."
                        }
                    }
                    Repeater {
                        model: root.joined ? (root.counter.feed || []) : []
                        delegate: Rectangle {
                            id: feedRow
                            required property var modelData
                            readonly property bool fresh: Date.now() - modelData.at < 60000
                            Layout.fillWidth: true
                            implicitHeight: 72
                            radius: Theme.spacing.radiusXlarge
                            color: fresh ? Theme.colors.getColor(Theme.palette.success, 0.16) : Theme.palette.backgroundElevated
                            border.color: fresh ? Theme.palette.success : Theme.palette.borderSecondary
                            border.width: fresh ? 2 : 1
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spacing.xlarge
                                anchors.rightMargin: Theme.spacing.xlarge
                                LogosText {
                                    Layout.fillWidth: true
                                    text: "+" + root.fmt(feedRow.modelData.amount) + " " + root.sym
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.titleText
                                    font.weight: Theme.typography.weightBold
                                }
                                LogosText {
                                    text: (feedRow.fresh ? "Just paid · " : "") + root.clockOf(feedRow.modelData.at)
                                    color: feedRow.fresh ? Theme.palette.success : Theme.palette.textSecondary
                                    font.pixelSize: Theme.typography.primaryText
                                    font.weight: Theme.typography.weightMedium
                                }
                            }
                        }
                    }
                    Caption {
                        visible: root.joined && (root.counter.feed || []).length === 0
                        horizontalAlignment: Text.AlignHCenter
                        text: "Waiting for the first payment…"
                    }
                    Caption {
                        visible: root.joined && (root.counter.feed || []).length > 0
                        horizontalAlignment: Text.AlignHCenter
                        text: "Private payments look the same as ordinary ones here — an amount, never a name. Two payments in the same block show as one line."
                    }
                    LinkText {
                        Layout.alignment: Qt.AlignHCenter
                        visible: (root.counter.feed || []).length > 0
                        text: "Clear the list"
                        onClicked: root.callArgs("clearFeed", [])
                    }
                }

                // ═══ ORGANIZER ═══════════════════════════════════════
                ColumnLayout {
                    visible: root.ready && root.page === "organizer"
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    Card {
                        visible: !root.org.active
                        Heading {
                            text: "Start a station"
                        }
                        Caption {
                            text: "Creates your residency's token with a fixed supply, a treasury that holds it, and the shop's till. Only this wallet will ever be able to create more — faucet tokens can't be spent here. Takes 2–3 minutes."
                        }
                        Field {
                            id: tokenName
                            text: "FIELD"
                            placeholderText: "Token name"
                        }
                        Field {
                            id: tokenSupply
                            text: "30000"
                            placeholderText: "Total supply"
                            textInput.validator: IntValidator {
                                bottom: 1
                            }
                        }
                        Action {
                            Layout.fillWidth: true
                            text: "Create"
                            enabled: tokenName.text.trim().length > 0 && root.num(tokenSupply.text) > 0
                            busy: root.jobBusy("create")
                            onClicked: {
                                root.lastError = "";
                                root.callArgs("createStation", [tokenName.text.trim(), tokenSupply.text.trim()]);
                            }
                        }
                    }

                    RowLayout {
                        visible: root.org.active === true
                        Layout.fillWidth: true
                        spacing: Theme.spacing.medium
                        Repeater {
                            model: [["Supply", root.fmt(root.station.supply)], ["In the treasury", root.fmt(root.org.treasuryBalance)], ["Welcomed", String((root.org.issued || []).length)]]
                            delegate: Card {
                                id: statCard
                                required property var modelData
                                pad: Theme.spacing.large
                                LogosText {
                                    text: statCard.modelData[0]
                                    color: Theme.palette.textTertiary
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                                LogosText {
                                    text: statCard.modelData[1]
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.panelTitleText
                                    font.weight: Theme.typography.weightBold
                                }
                            }
                        }
                    }

                    Card {
                        visible: root.org.active === true
                        Heading {
                            text: "Welcome a resident"
                        }
                        Caption {
                            text: "Paste the payment link from their Receive screen. They also get a little LGO so ordinary payments just work."
                        }
                        Field {
                            id: issueTo
                            placeholderText: "Their payment link"
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small
                            Field {
                                id: issueAmount
                                text: "1000"
                                Layout.preferredWidth: 120
                                Layout.fillWidth: false
                                textInput.validator: IntValidator {
                                    bottom: 1
                                }
                            }
                            LogosText {
                                Layout.fillWidth: true
                                text: root.sym
                                color: Theme.palette.textSecondary
                            }
                            Action {
                                text: "Send welcome " + root.sym
                                enabled: issueTo.text.trim().length > 30 && root.num(issueAmount.text) > 0
                                busy: root.jobBusy("issue")
                                onClicked: {
                                    root.lastError = "";
                                    root.callArgs("issue", [issueTo.text.trim(), issueAmount.text.trim(), true], function (r) {
                                        if (r && r.ok)
                                            issueTo.text = "";
                                    });
                                }
                            }
                        }
                        Repeater {
                            model: (root.st.jobs || []).filter(function (j) {
                                return (j.kind === "issue" || j.kind === "mint") && (root.inProgress(j.status) || Date.now() - j.finished < 300000);
                            }).slice(0, 3)
                            delegate: LogosText {
                                required property var modelData
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                text: modelData.status === "done" ? "✓ " + modelData.label : modelData.status === "failed" ? "✕ " + modelData.label + " — " + modelData.error : "… " + modelData.label.replace(/^Issued/, "Sending").replace(/^Minted/, "Minting") + " — confirming, usually under a minute"
                                color: modelData.status === "done" ? Theme.palette.success : modelData.status === "failed" ? Theme.palette.error : Theme.palette.warning
                                font.pixelSize: Theme.typography.secondaryText
                            }
                        }
                    }

                    Card {
                        visible: root.org.active === true
                        Heading {
                            text: "Station code"
                        }
                        Caption {
                            text: "New arrivals paste this into Field Pay to join. It contains no keys, so post it anywhere."
                        }
                        CopyBox {
                            value: root.station.code || ""
                            key: "code2"
                        }
                    }

                    Card {
                        visible: root.org.active === true
                        Heading {
                            text: "Mint more"
                        }
                        Caption {
                            text: "Adds " + root.sym + " to the treasury — for a late arrival, or to replace a lost wallet's balance."
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.small
                            Field {
                                id: mintAmount
                                placeholderText: "Amount"
                                textInput.validator: IntValidator {
                                    bottom: 1
                                }
                            }
                            Action {
                                primary: false
                                text: "Mint"
                                busy: root.jobBusy("mint")
                                enabled: root.num(mintAmount.text) > 0
                                onClicked: {
                                    root.lastError = "";
                                    root.callArgs("mint", [mintAmount.text.trim()], function (r) {
                                        if (r && r.ok)
                                            mintAmount.text = "";
                                    });
                                }
                            }
                        }
                    }

                    Card {
                        visible: root.org.active === true && (root.org.issued || []).length > 0
                        Heading {
                            text: "Welcomed"
                        }
                        Repeater {
                            model: root.org.issued || []
                            delegate: RowLayout {
                                id: wl
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.spacing.medium
                                LogosText {
                                    Layout.fillWidth: true
                                    text: wl.modelData.who || root.shortAddr(wl.modelData.to)
                                    font.family: wl.modelData.who ? Theme.typography.publicSans : "Menlo"
                                    color: Theme.palette.text
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                                LogosText {
                                    text: root.fmt(wl.modelData.amount) + " · " + root.whenOf(wl.modelData.at)
                                    color: Theme.palette.textTertiary
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                            }
                        }
                    }
                }

                Item {
                    implicitHeight: Theme.spacing.xxlarge
                }
            }
        }
    }
}
