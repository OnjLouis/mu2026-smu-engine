// Tiny IndexedDB wrapper: persist ROM files locally so they survive reloads.
// Everything stays in this browser; nothing is uploaded anywhere.
const databaseName = "s-mu2000-web";
const storeName = "roms";

export interface StoredRom {
    name: string;
    data: ArrayBuffer;
}

function openDatabase(): Promise<IDBDatabase> {
    return new Promise((resolve, reject) => {
        const request = indexedDB.open(databaseName, 1);
        request.addEventListener("upgradeneeded", () => {
            request.result.createObjectStore(storeName);
        });
        request.addEventListener("success", () => resolve(request.result));
        request.addEventListener("error", () => {
            reject(request.error ?? new Error("cannot open rom storage"));
        });
    });
}

function requestToPromise<T>(request: IDBRequest<T>): Promise<T> {
    return new Promise((resolve, reject) => {
        request.addEventListener("success", () => resolve(request.result));
        request.addEventListener("error", () => {
            reject(request.error ?? new Error("rom storage failed"));
        });
    });
}

async function withDatabase<T>(
    run: (database: IDBDatabase) => Promise<T>
): Promise<T> {
    const database = await openDatabase();
    try {
        return await run(database);
    } finally {
        database.close();
    }
}

function completeTransaction(tx: IDBTransaction): Promise<void> {
    return new Promise((resolve, reject) => {
        tx.addEventListener("complete", () => resolve());
        const fail = () => {
            reject(tx.error ?? new Error("rom storage failed"));
        };
        tx.addEventListener("error", fail);
        tx.addEventListener("abort", fail);
    });
}

function isStoredRom(row: unknown): row is StoredRom {
    return (
        typeof row === "object" &&
        row !== null &&
        "name" in row &&
        typeof row.name === "string" &&
        "data" in row &&
        row.data instanceof ArrayBuffer
    );
}

export async function loadStoredRoms(): Promise<StoredRom[]> {
    return withDatabase(async (database) => {
        const store = database
            .transaction(storeName, "readonly")
            .objectStore(storeName);
        const rows: unknown = await requestToPromise(store.getAll());
        return Array.isArray(rows) ? rows.filter(isStoredRom) : [];
    });
}

export async function storeRoms(files: File[]): Promise<void> {
    const roms: StoredRom[] = [];
    for (const file of files) {
        roms.push({ name: file.name, data: await file.arrayBuffer() });
    }
    await withDatabase(async (database) => {
        const tx = database.transaction(storeName, "readwrite");
        const store = tx.objectStore(storeName);
        const done = completeTransaction(tx);
        store.clear();
        for (const rom of roms) store.put(rom, rom.name);
        await done;
    });
}

export async function clearStoredRoms(): Promise<void> {
    await withDatabase(async (database) => {
        const tx = database.transaction(storeName, "readwrite");
        const done = completeTransaction(tx);
        tx.objectStore(storeName).clear();
        await done;
    });
}
