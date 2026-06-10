# VortexOS 🌀

**VortexOS** adalah sistem operasi *hard real-time* berkinerja ekstrem yang dibangun dari nol menggunakan **C++23 modern** untuk arsitektur x86-64. Fokus utama proyek ini adalah latensi interupsi ultra-rendah, determinisme maksimal, dan keamanan memori tingkat tinggi melalui sistem tipe data yang kuat.

## 🚀 Fitur Unggulan (Ultra-Expert Specs)

-   **Real-Time Scheduler (SCHED-RT):** Implementasi algoritma *Priority-based* (0-99) dan *Earliest Deadline First* (EDF) untuk jaminan determinisme tugas.
-   **Advanced Memory Management:**
    -   **Buddy System PMM:** Alokasi memori fisik $O(1)$ untuk performa real-time.
    -   **Slab Allocator:** Manajemen objek kernel yang efisien untuk meminimalkan fragmentasi.
    -   **Strong Types:** Penggunaan `PhysAddr`, `VirtAddr`, dan `UserPtr` untuk mencegah kebocoran data dan kesalahan memori di level kompilasi.
-   **Multiprocessing (SMP):** Mendukung skalabilitas multi-core melalui bootstrap APIC (INIT-SIPI-SIPI).
-   **Zero-Copy Networking:** Framework stack jaringan yang dirancang untuk transmisi data berkecepatan tinggi tanpa overhead penyalinan memori.
-   **VFS & RAMFS:** Abstraksi sistem berkas virtual untuk integrasi berbagai media penyimpanan.

## 🛠️ Tumpukan Teknologi

-   **Bahasa:** C++23 (Modern & Freestanding), NASM (Assembly)
-   **Compiler:** Clang 17 / LLVM (Target: `x86_64-pc-none-elf`)
-   **Build System:** CMake & Ninja
-   **Bootloader:** Limine (Base Revision 3)
-   **Testing:** KTEST Framework untuk audit latensi nanodetik.

## 📦 Cara Membangun

### Prasyarat
-   Windows dengan PowerShell
-   LLVM/Clang 17+
-   NASM
-   CMake & Ninja

### Langkah-langkah
1.  Clone repositori ini:
    ```bash
    git clone https://github.com/wi5nuu/VortexOS.git
    cd VortexOS
    ```
2.  Jalankan skrip build otomatis:
    ```powershell
    ./build.ps1
    ```
3.  Hasil kompilasi akan berada di direktori `build/vortexos.elf`.

## 🗺️ Roadmap Pengembangan

-   [x] **Phase 1-2:** Fondasi Kernel, RT-Memory, SMP, & Scheduler.
-   [ ] **Phase 3:** Userland Isolation (ELF Loader, Syscalls, KPTI).
-   [ ] **Phase 4-5:** VFS Implementation & Driver Framework (PCI/NVMe).
-   [ ] **Phase 6:** Network Stack (UDP/IP).
-   [ ] **Phase 8:** Security Hardening (KASLR, Stack Protectors).

---

**VortexOS** — *Built for speed, engineered for precision.*

© 2026 VortexOS Project by [@wi5nuu](https://github.com/wi5nuu)
