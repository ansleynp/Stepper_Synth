"""Stepper Synth desktop companion. Run with python desktop_app/main.py."""
import queue
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import midi_files as files
from harmony import (HIGH, LOW, TONICS, SCALES, FAMILIES, chord, identify, interval_name, pitch,
                     scale_pitch)
from preview_audio import PreviewAudio
from serial_link import MAX_FILE, SerialLink, available_ports

INK, MUTED, BG, PAPER = "#152e32", "#4e6768", "#edf2ef", "#ffffff"
TEAL, LIME, LINE = "#126965", "#c3f185", "#cfddda"
ACCENTS = ("#bbeba0", "#8ddbd1", "#ffd191")
# Synth transport buttons: request op, button label, keyboard shortcut.
TRANSPORT = (("record", "● Record / Stop", "1"), ("play", "▶ Play / Pause", "2"),
             ("reset", "⟲ Reset", "3"))


def label(parent, text, size=11, color=INK, bold=False, bg=None):
    return tk.Label(parent, text=text, anchor="w", bg=bg or parent["bg"], fg=color,
                    font=("Helvetica", size, "bold" if bold else "normal"), bd=0)


def button(parent, text, action, primary=False):
    return ttk.Button(parent, text=text, command=action,
                      style="Primary.TButton" if primary else "TButton")


class Studio:
    def __init__(self, root):
        self.root = root
        root.title("Stepper Synth Studio")
        root.geometry("1010x735")
        root.minsize(840, 690)
        root.configure(bg=BG)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure(".", font=("Helvetica", 11))
        style.configure("TButton", padding=(10, 6), background=PAPER, foreground=INK)
        style.configure("Primary.TButton", background=TEAL, foreground="white")
        style.map("Primary.TButton", background=[("disabled", LINE), ("active", "#075651")],
                  foreground=[("disabled", MUTED)])
        style.configure("Treeview", rowheight=32, background=PAPER, fieldbackground=PAPER)
        style.configure("Treeview.Heading", font=("Helvetica", 10, "bold"))
        self.audio = PreviewAudio()
        self.link = None
        self.busy = False
        self.caps = set()
        self.active = None
        self.pending = None
        self.board_mode = "ordinary"
        self.live = None
        self.selected = 60
        self.held = set()
        self.transport = None   # last transport event from the board
        self.limits = {}        # max_events / max_file from the board's hello
        self.key = tk.StringVar(value="C")
        self.scale = tk.StringVar(value="Major")
        self.family = tk.StringVar(value="Triad")
        self.mode = tk.StringVar(value="Auto")
        self.port = tk.StringVar()
        self.header()
        self.body = tk.Frame(root, bg=BG)
        self.body.pack(fill="both", expand=True, padx=22, pady=(14, 0))
        self.chords = tk.Frame(self.body, bg=BG)
        self.records = tk.Frame(self.body, bg=BG)
        self.chord_page()
        self.record_page()
        self.status = label(root, "", 10, MUTED)
        self.status.pack(fill="x", padx=22, pady=8)
        self.status.configure(text="Offline preview · Connect a compatible ESP32 for live motor feedback.")
        self.scan()
        self.page("chord")
        self.refresh()
        self.render()
        self.controls()
        for op, _, key in TRANSPORT:  # keys 1/2/3 press the synth's buttons
            root.bind(f"<KeyPress-{key}>",
                      lambda e, o=op: None if isinstance(e.widget, ttk.Entry) else self.press(o))
        root.after(80, self.poll)
        root.protocol("WM_DELETE_WINDOW", self.close)

    def header(self):
        head = tk.Frame(self.root, bg=INK)
        head.pack(fill="x")
        label(head, "Stepper Synth", 22, PAPER, True).pack(side="left", padx=22, pady=12)
        self.nav = {}
        for tag, title in [("chord", "Live Chord Mode"), ("record", "Recordings")]:
            w = label(head, title, 12, PAPER, True)
            w.configure(cursor="hand2", padx=12, pady=10, takefocus=1)
            for event in ("<Button-1>", "<Return>", "<space>"):
                w.bind(event, lambda e, t=tag: self.page(t))
            w.pack(side="left", padx=2)
            self.nav[tag] = w
        bar = tk.Frame(self.root, bg=PAPER, padx=22, pady=8)
        bar.pack(fill="x")
        label(bar, "ESP32", 11, INK, True).pack(side="left")
        self.indicator = label(bar, "Offline", 10, MUTED)
        self.indicator.pack(side="left", padx=12)
        self.connect_btn = button(bar, "Connect", self.connect, True)
        self.connect_btn.pack(side="right")
        button(bar, "Refresh ports", self.scan).pack(side="right", padx=6)
        self.ports = ttk.Combobox(bar, textvariable=self.port, state="readonly", width=26)
        self.ports.pack(side="right")

    def page(self, tag):
        self.chords.pack_forget()
        self.records.pack_forget()
        (self.chords if tag == "chord" else self.records).pack(fill="both", expand=True)
        for name, w in self.nav.items():
            w.configure(bg=LIME if name == tag else INK, fg=INK if name == tag else PAPER)

    def chord_page(self):
        row = tk.Frame(self.chords, bg=BG)
        row.pack(fill="x", pady=(0, 10))
        for title, var, values, width in [
            ("Mode", self.mode, ("Auto", "Identify"), 9), ("Key", self.key, TONICS, 5),
            ("Scale", self.scale, list(SCALES), 13), ("Voicing", self.family, FAMILIES, 10)]:
            box = tk.Frame(row, bg=BG)
            box.pack(side="left", padx=(0, 10))
            label(box, title, 10, MUTED).pack(anchor="w")
            combo = ttk.Combobox(box, textvariable=var, values=values, state="readonly", width=width)
            combo.pack()
            combo.bind("<<ComboboxSelected>>", lambda e: self.render())
        self.exit_btn = button(row, "Exit live", lambda: self.job("ordinary"))
        self.exit_btn.pack(side="right", anchor="s")
        self.apply_btn = button(row, "Apply to synth", self.apply, True)
        self.apply_btn.pack(side="right", anchor="s", padx=6)
        card = tk.Frame(self.chords, bg=INK, padx=20, pady=14)
        card.pack(fill="x")
        self.source = label(card, "", 10, LIME, True)
        self.source.pack(anchor="w")
        self.name = label(card, "", 32, PAPER, True)
        self.name.pack(anchor="w")
        self.details = label(card, "", 11, "#c7dcd8")
        self.details.pack(anchor="w")
        motors = tk.Frame(self.chords, bg=BG)
        motors.pack(fill="x", pady=10)
        self.voices = []
        for i, accent in enumerate(ACCENTS):
            tile = tk.Frame(motors, bg=PAPER, padx=12, pady=8)
            tile.pack(side="left", expand=True, fill="both", padx=(0, 6 if i<2 else 0))
            tk.Frame(tile, bg=accent, width=4).pack(side="left", fill="y", padx=(0,10))
            label(tile, f"Motor {i+1}", 10, MUTED, True).pack(anchor="w")
            value = label(tile, "", 18, INK, True)
            value.pack(anchor="w")
            self.voices.append(value)
        panel = tk.Frame(self.chords, bg=PAPER, padx=16, pady=10)
        panel.pack(fill="both", expand=True)
        self.kbtitle = label(panel, "", 13, INK, True)
        self.kbtitle.pack(anchor="w")
        self.canvas = tk.Canvas(panel, height=90, bg=PAPER, highlightthickness=0,
                                cursor="hand2", takefocus=1)
        self.canvas.pack(fill="both", expand=True, pady=(8,4))
        self.canvas.bind("<Configure>", lambda e:self.render())
        self.canvas.bind("<Button-1>", self.click_note)
        self.canvas.bind("<ButtonRelease-1>", lambda e: self.audio.stop())
        self.root.bind("<ButtonRelease-1>", lambda e: self.audio.stop(), add="+")
        self.canvas.bind("<FocusOut>", lambda e: self.audio.stop())
        self.hint = label(panel, "", 10, MUTED)
        self.hint.pack(anchor="w")
        self.keyblocks = []

    def record_page(self):
        row = tk.Frame(self.records, bg=BG)
        row.pack(fill="x", pady=(0,12))
        label(row, "Recording library", 22, INK, True).pack(side="left")
        button(row, "Create demo MIDI", lambda:self.local(files.create_demo_recording)).pack(side="right")
        button(row, "Import MIDI…", self.import_file, True).pack(side="right", padx=6)
        card = tk.Frame(self.records, bg=INK, padx=18, pady=12)
        card.pack(fill="x", pady=(0, 12))
        label(card, "SYNTH TRANSPORT  ·  keys 1 / 2 / 3", 10, LIME, True).pack(anchor="w")
        line = tk.Frame(card, bg=INK)
        line.pack(fill="x")
        self.transport_label = label(line, "", 16, PAPER, True)
        self.transport_label.pack(side="left", pady=(4, 0))
        self.transport_btns = []
        for op, text, key in reversed(TRANSPORT):
            b = button(line, f"{text}  [{key}]", lambda o=op: self.press(o), op == "record")
            b.pack(side="right", padx=(6, 0))
            self.transport_btns.append(b)
        self.transport_hint = label(card, "", 10, "#c7dcd8")
        self.transport_hint.pack(anchor="w")
        self.tree = ttk.Treeview(self.records, columns=("name","duration","notes","tracks","remove"), show="headings", selectmode="browse")
        for key, title, width in [("name","Recording",380),("duration","Length",85),
                                  ("notes","Notes",65),("tracks","Tracks",65),("remove","Remove",70)]:
            self.tree.heading(key, text=title, anchor="center" if key=="remove" else "w")
            self.tree.column(key, width=width, stretch=key=="name", anchor="center" if key=="remove" else "w")
        scroll = ttk.Scrollbar(self.records, command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        area = tk.Frame(self.records, bg=BG)
        area.pack(side="bottom", fill="x", pady=10)
        self.count = label(area, "", 10, MUTED)
        self.count.pack(side="left")
        self.loadbtn = button(area, "Load onto synth", self.load_file)
        self.loadbtn.pack(side="right")
        self.savebtn = button(area, "Save from synth…", lambda:self.job("save"))
        self.savebtn.pack(side="right", padx=6)
        tools = tk.Frame(self.records, bg=PAPER, padx=14, pady=8)
        tools.pack(side="bottom", fill="x", pady=(10, 0))
        label(tools, "Transpose", 11, INK, True).pack(side="left")
        self.transpose = tk.IntVar(value=0)
        spin = ttk.Spinbox(tools, from_=-24, to=24, increment=1, width=4, state="readonly",
                           textvariable=self.transpose, command=self.show_transpose)
        spin.pack(side="left", padx=(10, 4))
        label(tools, "semitones", 10, MUTED).pack(side="left")
        self.transpose_label = label(tools, "", 10, TEAL, True)
        self.transpose_label.pack(side="left", padx=12)
        self.copybtn = button(tools, "Save transposed copy", self.save_transposed)
        self.copybtn.pack(side="right")
        button(tools, "Reset", lambda: (self.transpose.set(0), self.show_transpose())).pack(
            side="right", padx=6)
        self.show_transpose()
        scroll.pack(side="right", fill="y")
        self.tree.pack(fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", lambda e:self.controls())
        self.tree.bind("<ButtonRelease-1>", self.tree_click)
        self.tree.bind("<Motion>", lambda e: self.tree.configure(
            cursor="hand2" if self.on_remove(e) else ""))
        for key in ("<Delete>", "<BackSpace>"):
            self.tree.bind(key, lambda _e: self.remove_file(self.tree.focus()))
        self.renaming = None  # (entry widget, original file name) while editing a name
        self.tree.bind("<Double-1>", self.start_rename)
        self.tree.bind("<MouseWheel>", lambda _e: self.finish_rename(True), add="+")
        self.show_transport()

    def start_rename(self, event):
        """Double-click a name: edit it in place. Enter/click away saves, Escape cancels."""
        if (self.tree.identify_region(event.x, event.y) != "cell"
                or self.tree.identify_column(event.x) != "#1"):
            return
        name = self.tree.identify_row(event.y)
        bbox = name and self.tree.bbox(name, "name")
        if not bbox:
            return
        self.finish_rename(True)
        entry = ttk.Entry(self.tree)
        entry.insert(0, name)
        entry.select_range(0, len(name.rsplit(".", 1)[0]))  # select the name, not ".mid"
        entry.icursor(len(name.rsplit(".", 1)[0]))
        x, y, width, height = bbox
        entry.place(x=x, y=y, width=width, height=height)
        entry.focus_set()
        for key in ("<Return>", "<KP_Enter>", "<FocusOut>"):
            entry.bind(key, lambda _e: self.finish_rename(True))
        entry.bind("<Escape>", lambda _e: self.finish_rename(False))
        self.renaming = (entry, name)

    def finish_rename(self, commit):
        if not self.renaming:
            return
        (entry, old), self.renaming = self.renaming, None  # clear first: destroy fires FocusOut
        new = entry.get().strip()
        entry.destroy()
        self.tree.focus_set()
        if not commit or new in ("", old):
            return
        try:
            dest = files.rename_recording(old, new)
            self.status.configure(text=f"Renamed {old} → {dest.name}")
            self.refresh(dest)
        except Exception as exc:
            messagebox.showerror("Rename recording", str(exc))
            self.refresh(files.RECORDINGS / old)

    def on_remove(self, event):
        """True if the pointer is on a row's ✕ Remove cell."""
        return (self.tree.identify_region(event.x, event.y) == "cell"
                and self.tree.identify_column(event.x) == "#5")

    def tree_click(self, event):
        if self.on_remove(event):
            self.remove_file(self.tree.identify_row(event.y))

    def remove_file(self, name):
        if not name or not self.tree.exists(name):
            return
        if not messagebox.askyesno("Remove recording",
                f"Delete {name}?\n\nThe file is permanently removed from desktop_app/recordings.",
                icon="warning", default="no"):
            return
        try:
            files.remove_recording(name)
            self.status.configure(text=f"Removed {name}")
        except Exception as exc:
            messagebox.showerror("Remove recording", str(exc))
        self.refresh()

    def show_transport(self):
        t = self.transport
        if not self.link or "transport" not in self.caps:
            text, hint = "Synth not connected", "Connect a board running the looper firmware to record, play and reset."
        elif t is None:
            text, hint = "Waiting for synth…", ""
        else:
            state, length = t.get("state", "?"), t.get("length_ms", 0) / 1000
            events = t.get("events", 0)
            text = {"recording": f"● Recording  ·  {events} events",
                    "playing": f"▶ Playing  ·  {length:.1f} s loop",
                    "paused": f"❚❚ Paused at {t.get('pos_ms', 0)/1000:.1f} s of {length:.1f} s",
                    }.get(state, f"■ Stopped  ·  {length:.1f} s take" if length else "■ No take on the synth")
            hint = ("The take starts on the first note. Press Record again to finish." if state == "recording"
                    else f"{events} note events in SRAM · loops without the computer while powered." if length
                    else "Press Record and play, or load a MIDI file below.")
        self.transport_label.configure(text=text)
        self.transport_hint.configure(text=hint)

    def press(self, op):
        if not self.link or "transport" not in self.caps:
            self.status.configure(text="Connect the synth to use Record / Play / Reset.")
        elif self.busy:
            self.status.configure(text="Synth is busy · try again in a moment.")
        else:
            self.job(op)

    def refresh(self, chosen=None):
        self.finish_rename(False)  # rows are rebuilt; drop any half-typed name
        for item in self.tree.get_children():
            self.tree.delete(item)
        for p in files.list_recordings():
            try:
                tracks, notes, duration = files.midi_info(p)
            except Exception:
                tracks, notes, duration = "—","—","Unreadable"
            self.tree.insert("", "end", iid=p.name, values=(p.name,duration,notes,tracks,"✕"))
        if chosen and self.tree.exists(chosen.name):
            self.tree.selection_set(chosen.name)
            self.tree.see(chosen.name)
        total = len(self.tree.get_children())
        self.count.configure(text=f"{total} local recording{'s' if total != 1 else ''}"
                                  "  ·  double-click a name to rename")
        self.controls()

    def local(self, action):
        try:
            p = action()
            self.refresh(p)
            self.status.configure(text=f"Saved locally · {p.name}")
        except Exception as exc:
            messagebox.showerror("Recording", str(exc))

    def import_file(self):
        p = filedialog.askopenfilename(filetypes=[("MIDI files","*.mid *.midi *.MID *.MIDI"),("All files","*")])
        if p:
            self.local(lambda:files.import_midi(p))

    def load_file(self):
        sel = self.tree.selection()
        if not sel:
            return
        try:
            p = files.RECORDINGS / sel[0]
            if p.stat().st_size > MAX_FILE:
                raise ValueError("File exceeds the 256 KiB desktop transfer limit.")
            shift = self.transpose.get()
            max_events, max_file = self.limits.get("max_events"), self.limits.get("max_file", MAX_FILE)
            take = files.board_bytes(p, shift, max_events)
            if len(take.data) > max_file:
                raise ValueError(f"{p.name} is {len(take.data)//1024} KB after conversion; the synth accepts {max_file//1024} KB.")
            cut = (f"\n\nToo long for the synth: {take.full_events} note events, it holds {max_events}. "
                   f"Only the first {take.seconds:.1f} s of {take.full_seconds:.1f} s will be loaded and looped."
                   if take.truncated else "")
            if messagebox.askyesno("Load recording",
                    f"Replace the take on the synth with {p.name}?\n\n"
                    f"{take.events} note events · {take.seconds:.1f} s loop"
                    + (f" · transposed {shift:+d} ({interval_name(shift)})" if shift else "")
                    + (f"\n{take.folded} note events folded by octaves to stay in MIDI range." if take.folded else "")
                    + cut + "\n\nPlayback stops; press Play to start it.",
                    icon="warning" if take.truncated else "question"):
                self.job("load", take.data)
        except Exception as exc:
            messagebox.showerror("Load recording", str(exc))

    def show_transpose(self):
        shift = self.transpose.get()
        self.transpose_label.configure(text=interval_name(shift) +
                                       ("  ·  applied when loading onto the synth" if shift else ""))
        self.controls()

    def save_transposed(self):
        sel, shift = self.tree.selection(), self.transpose.get()
        if not sel or not shift:
            return
        try:
            dest, folded = files.transpose_copy(files.RECORDINGS / sel[0], shift)
            self.refresh(dest)
            self.status.configure(text=f"Saved {dest.name} · {interval_name(shift)}"
                                       + (f" · {folded} notes folded by octaves" if folded else ""))
        except Exception as exc:
            messagebox.showerror("Transpose", str(exc))

    def render(self):
        auto = self.mode.get() == "Auto"
        c = chord(self.selected, self.key.get(), self.scale.get(), self.family.get())
        notes = c.notes if auto else tuple(sorted(self.held))[:3]
        roles = c.roles if auto else ("Held",)*len(notes)
        title = c.name if auto else identify(self.held)
        text = (f"Root {scale_pitch(self.selected, self.key.get(), self.scale.get())}    /    Degree {c.degree}    /    {c.detail}"
                if auto else "Held keys   " + (" · ".join(map(pitch,sorted(self.held))) or "—"))
        spelling = (self.key.get(), self.scale.get()) if auto else None
        source = "LOCAL PREVIEW"
        if self.live is not None:
            spelling = self.active[:2] if self.board_mode == "map" and self.active else None
            notes = self.live["notes"]
            root = self.live.get("root")
            roles = ("Sounding",)*len(notes)
            held = self.live.get("held", [])
            title = identify(held if self.board_mode == "identify" else notes)
            text = "Held keys   " + (" · ".join(map(pitch, held)) or "—")
            if self.board_mode == "map" and self.active and root in range(LOW,HIGH+1):
                mapped = chord(root, *self.active)
                if tuple(notes) == mapped.notes:
                    title, roles = mapped.name, mapped.roles
                    text = f"Root {scale_pitch(root, *self.active[:2])}    /    Degree {mapped.degree}    /    {mapped.detail}"
            source = "ESP32  /  REPORTED VOICES"
        self.source.configure(text=source)
        self.name.configure(text=title, wraplength=max(740,self.root.winfo_width()-120))
        self.details.configure(text=text,wraplength=max(740,self.root.winfo_width()-120))
        def displayed_pitch(n):
            return scale_pitch(n, *spelling) if spelling else pitch(n)

        for i, w in enumerate(self.voices):
            w.configure(text=f"{displayed_pitch(notes[i])}   ·   {roles[i]}" if i < len(notes) else "—")
        self.kbtitle.configure(text="Motor pitches" if self.live is not None else "Keyboard preview")
        self.hint.configure(text=("Board-reported pitches · A2–C6" if self.live is not None else
            "Press and hold a piano key to hear its chord · A2–C6" if auto else
            "Click keys to add or remove held notes. Colors show up to three preview voices."))
        self.draw(notes, displayed_pitch)

    def draw(self, notes, displayed_pitch):
        cv = self.canvas
        cv.delete("all")
        self.keyblocks = []
        white = [n for n in range(LOW,HIGH+1) if n%12 in (0,2,4,5,7,9,11)]
        width = max(1,cv.winfo_width()-2) / len(white)
        h = max(60,cv.winfo_height()-2)
        colors = dict(zip(notes,ACCENTS))
        def key(n,x1,x2,y,black):
            color = colors.get(n, INK if black else "#f7faf7")
            cv.create_rectangle(x1,1,x2,y,fill=color,outline=INK if black else LINE)
            if n%12==0 or n in colors:
                cv.create_text((x1+x2)/2,y-10,text=displayed_pitch(n),font=("Helvetica",8,"bold"),fill=INK)
            self.keyblocks.append((x1,x2,y,n))
        for i,n in enumerate(white):
            key(n,i*width+1,(i+1)*width+1,h,False)
        for i,n in enumerate(white[:-1]):
            if white[i+1]-n==2:
                key(n+1,(i+.69)*width+1,(i+1.31)*width+1,h*.59,True)

    def click_note(self,event):
        if self.live is not None:
            return
        for x1,x2,y,n in reversed(self.keyblocks):
            if x1<=event.x<=x2 and event.y<=y:
                self.selected = n
                if self.mode.get()=="Identify":
                    self.held.symmetric_difference_update({n})
                self.canvas.focus_set()
                self.render()
                notes = (chord(n, self.key.get(), self.scale.get(), self.family.get()).notes
                         if self.mode.get() == "Auto" else sorted(self.held)[:3])
                try:
                    self.audio.play(notes)
                except (ImportError, RuntimeError, OSError) as exc:
                    self.status.configure(text=f"Preview audio unavailable · {exc}")
                break

    def scan(self):
        ports = available_ports()
        self.ports.configure(values=ports)
        if self.port.get() not in ports:
            self.port.set(ports[0] if ports else "")
        self.controls()

    def controls(self):
        if not hasattr(self,"loadbtn"):
            return
        free = not self.busy
        available = free and self.board_mode=="ordinary" and "midi_file" in self.caps
        for widget, enabled in [(self.apply_btn,free and "chords" in self.caps),
            (self.exit_btn,free and self.board_mode!="ordinary"),
            (self.savebtn,available),(self.loadbtn,available and self.tree.selection()),
            (self.copybtn,bool(self.tree.selection()) and self.transpose.get() != 0),
            (self.connect_btn,bool(self.link) or bool(self.port.get()))] + [
            (b, free and bool(self.link) and "transport" in self.caps) for b in self.transport_btns]:
            widget.state(["!disabled" if enabled else "disabled"])

    def connect(self):
        if self.link:
            self.disconnect()
            self.status.configure(text="Disconnected")
            return
        if self.port.get():
            self.link = SerialLink()
            self.connect_btn.configure(text="Disconnect")
            self.indicator.configure(text="Connecting…")
            self.job("open", self.port.get())

    def disconnect(self):
        if self.link:
            self.link.stop_event.set()
        self.link = None
        self.caps.clear()
        self.limits = {}
        self.transport = None
        self.busy = False
        self.active = self.pending = self.live = None
        self.board_mode = "ordinary"
        self.indicator.configure(text="Offline")
        self.connect_btn.configure(text="Connect")
        self.show_transport()
        self.render()
        self.controls()

    def job(self, op, arg=None):
        if self.link and not self.busy:
            self.busy = True
            self.link.jobs.put((op,arg))
            self.status.configure(text="Waiting for ESP32…")
            self.controls()

    def apply(self):
        if self.mode.get()=="Identify":
            self.job("identify")
            return
        self.pending = (self.key.get(),self.scale.get(),self.family.get())
        mapping = [chord(n,*self.pending).notes for n in range(LOW,HIGH+1)]
        self.job("map", dict(first=LOW, rows=mapping))

    def poll(self):
        while self.link:
            try:
                op, result = self.link.events.get_nowait()
            except queue.Empty:
                break
            if op=="opened":
                self.indicator.configure(text="Port open · checking firmware")
                continue
            if op=="event" and result.get("event")=="transport":
                self.transport = result
                self.show_transport()
                continue
            if op=="event" and result.get("event")=="voices":
                notes, held = result.get("notes"), result.get("held",[])
                if (self.board_mode!="ordinary" and isinstance(notes,list) and len(notes)<=3
                    and all(type(x)==int and LOW<=x<=HIGH for x in notes)
                    and isinstance(held,list) and len(held)<=128
                    and all(type(x)==int and 0<=x<=127 for x in held)):
                    self.live = result
                    self.render()
                continue
            if op=="event":
                continue  # unknown event type from newer firmware
            self.busy = False
            if op=="lost":
                self.disconnect()
                self.status.configure(text=result)
            elif op=="error":
                self.status.configure(text=result)
            elif op=="open":
                caps = result.get("caps")
                if isinstance(caps,list):
                    self.caps = {c for c in caps if isinstance(c,str)}
                self.limits = {k: result[k] for k in ("max_events","max_file")
                               if type(result.get(k))==int and result[k] > 0}
                self.indicator.configure(text="Firmware connected")
                self.status.configure(text="ESP32 handshake acknowledged")
                self.show_transport()
            elif op in ("map","identify","ordinary"):
                self.board_mode = op
                self.active = self.pending if op=="map" else None
                self.live = None if op=="ordinary" else {"notes":[]}
                self.status.configure(text={"map":"Auto active · " + " / ".join(self.active or ()),
                    "identify":"Identify active","ordinary":"Ordinary keyboard play restored"}[op])
                self.render()
            elif op=="save":
                self.save_take(result)
            elif op=="load":
                self.status.configure(text=f"Loaded onto synth · {result.get('events','?')} note events, "
                                           f"{result.get('length_ms',0)/1000:.1f} s loop · press Play")
            elif op in ("record","play","reset"):
                self.status.configure(text="Synth acknowledged " + op)
            self.controls()
        self.root.after(80,self.poll)

    def save_take(self, data):
        dest = filedialog.asksaveasfilename(
            title="Save synth recording as MIDI", initialdir=files.ensure_folder(),
            initialfile=files.default_take_name(), defaultextension=".mid",
            filetypes=[("MIDI files","*.mid *.midi")])
        if not dest:
            self.status.configure(text="Save cancelled · the take is still on the synth")
            return
        self.local(lambda: files.save_bytes(data, dest))

    def close(self):
        self.audio.close()
        if self.link:
            self.link.stop_event.set()
        self.root.destroy()


if __name__ == "__main__":
    window = tk.Tk()
    Studio(window)
    window.mainloop()
