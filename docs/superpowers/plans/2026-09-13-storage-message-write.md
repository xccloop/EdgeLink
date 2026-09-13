# Storage Message Write Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store one `telemetry_sample_struct` as a fixed nine-byte record at a caller-specified GD25Q32 address.

**Architecture:** `storage_temperature()` owns only the conversion from the APP Message to its nine Flash bytes and forwards those bytes to the GD25Q32 BSP. The caller owns the address and decides when to erase a 4 KiB sector; this plan does not add address progression, recovery, or circular storage.

**Tech Stack:** C11, GD32F103, existing GD25Q32 SPI BSP.

---

### Task 1: Encode and write one temperature record

**Files:**
- Modify: `edgenode/User/App/Storage/storage.h`
- Modify: `edgenode/User/App/Storage/storage.c`
- Test: `edgenode` full firmware build

- [ ] **Step 1: Define the record length and public result values**

Add the following definitions and function declaration to `edgenode/User/App/Storage/storage.h`:

```c
#define STORAGE_SUCCESS 1U
#define STORAGE_FAIL    0U
#define STORAGE_TEMPERATURE_RECORD_LENGTH 9U

uint8_t storage_temperature(uint32_t address, const telemetry_sample_struct *message);
```

- [ ] **Step 2: Encode the Message into a fixed nine-byte array**

Change the private encoder in `edgenode/User/App/Storage/storage.c` to accept a Message pointer and write little-endian bytes with the cast after each shift:

```c
data[0] = (uint8_t)(message->sample_uptime_ms >> 0U);
data[1] = (uint8_t)(message->sample_uptime_ms >> 8U);
data[2] = (uint8_t)(message->sample_uptime_ms >> 16U);
data[3] = (uint8_t)(message->sample_uptime_ms >> 24U);
```

Encode `temperature` in bytes 4 through 7 with the same order, and put `temperature_scale` in byte 8.

- [ ] **Step 3: Write the encoded record without erasing**

Implement the public function as:

```c
uint8_t storage_temperature(uint32_t address, const telemetry_sample_struct *message)
{
    uint8_t data[STORAGE_TEMPERATURE_RECORD_LENGTH];

    if(message == 0)
    {
        return STORAGE_FAIL;
    }

    message_encode(data, message);
    if(gd25_write(address, data, STORAGE_TEMPERATURE_RECORD_LENGTH) == 0U)
    {
        return STORAGE_FAIL;
    }

    return STORAGE_SUCCESS;
}
```

Do not call `gd25_clear()` in this function because it erases the entire sector containing previous records.

- [ ] **Step 4: Build the complete firmware**

Run:

```powershell
mingw32-make -B all
```

Expected: exit code `0`; existing unrelated warnings may remain.

- [ ] **Step 5: Commit**

```powershell
git add edgenode/User/App/Storage/storage.h edgenode/User/App/Storage/storage.c docs/superpowers/plans/2026-09-13-storage-message-write.md
git commit -m "feat: encode message for flash storage"
```
