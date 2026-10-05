// Shared ROM picker card (render + live pages).
// Folder input, checklist and IndexedDB persistence live here.
// ROMs are picked once and stored locally.
import { clearStoredRoms, loadStoredRoms, storeRoms } from "./idb.ts";

export interface RomSlot {
    kind: number;
    fileName: string;
    label: string;
    isRequired: boolean;
}

export const ROM_SLOTS: RomSlot[] = [
    {
        kind: 0,
        fileName: "mu2000_flash.bin",
        label: "Program ROM (4 MB)",
        isRequired: true
    },
    {
        kind: 1,
        fileName: "xv364a0.ic49",
        label: "Wave ROM ic49 (8 MB)",
        isRequired: true
    },
    {
        kind: 2,
        fileName: "xv365a0.ic50",
        label: "Wave ROM ic50 (8 MB)",
        isRequired: true
    },
    {
        kind: 3,
        fileName: "xw848a0.ic53",
        label: "Wave ROM ic53 (8 MB)",
        isRequired: true
    },
    {
        kind: 4,
        fileName: "xw849a0.ic54",
        label: "Wave ROM ic54 (8 MB)",
        isRequired: true
    },
    {
        kind: 5,
        fileName: "sin-table.bin",
        label: "Sine table (64 KB, optional)",
        isRequired: false
    }
];

export interface RomCard {
    readonly files: Map<string, File>;
    refresh(): void;
    forget(): Promise<void>;
}

// Wire the card elements; stored ROMs load asynchronously first.
// The page updates its buttons in onChange after every change.
// Files ride along as an argument.
// The card itself does not exist yet the first time onChange runs.
export async function setupRomCard(
    input: HTMLInputElement,
    folderLabel: HTMLSpanElement,
    list: HTMLUListElement,
    forgetButton: HTMLButtonElement,
    onChange: (files: Map<string, File>) => void
): Promise<RomCard> {
    const files = new Map<string, File>();

    const refresh = () => {
        list.replaceChildren();
        for (const slot of ROM_SLOTS) {
            const item = document.createElement("li");
            const isFound = files.has(slot.fileName);
            item.textContent = `${isFound ? "✓" : slot.isRequired ? "✗" : "○"} ${slot.label} — ${slot.fileName}`;
            item.className = isFound
                ? "found"
                : slot.isRequired
                  ? "missing"
                  : "optional";
            list.append(item);
        }
        onChange(files);
    };

    const card: RomCard = {
        files,
        refresh,
        forget: async () => {
            try {
                await clearStoredRoms();
            } catch {
                // Storage may be unavailable.
                // Forgetting the session copies is still useful.
            }
            files.clear();
            folderLabel.textContent = "your roms/ directory";
            refresh();
        }
    };

    const adopt = (picked: File[], folder: string, shouldRemember: boolean) => {
        files.clear();
        for (const file of picked) {
            files.set(file.name.toLowerCase(), file);
        }
        folderLabel.textContent = folder;
        if (shouldRemember) {
            // Persist only the six ROM slots.
            // Other files stay session-only (basename collisions).
            const slotNames = new Set(
                ROM_SLOTS.map((slot) => slot.fileName)
            );
            const romsOnly = picked.filter((file) =>
                slotNames.has(file.name.toLowerCase())
            );
            void (async () => {
                try {
                    await storeRoms(romsOnly);
                } catch {
                    // Session-only use; the checklist still works.
                }
            })();
        }
        refresh();
    };

    input.addEventListener("change", () => {
        const picked = [...(input.files ?? [])];
        const first = picked[0]?.webkitRelativePath ?? "";
        adopt(picked, first.split("/", 1)[0] || "your roms/ directory", true);
    });

    forgetButton.addEventListener("click", () => {
        void card.forget();
    });

    try {
        const stored = await loadStoredRoms();
        if (stored.length > 0) {
            adopt(
                stored.map((rom) => new File([rom.data], rom.name)),
                "stored locally",
                false
            );
        }
    } catch {
        // No stored ROMs (or no storage); the picker covers it.
    }

    refresh();
    return card;
}
