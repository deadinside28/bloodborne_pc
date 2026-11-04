// Focus the game window (caption starts with "Bloodborne") and report the active window.
const wins = workspace.windowList();
for (let i = 0; i < wins.length; i++) {
    if (wins[i].caption.indexOf("Bloodborne") === 0) {
        workspace.activeWindow = wins[i];
    }
}
print("BBFOCUS active=" + (workspace.activeWindow ? workspace.activeWindow.caption : "none"));
