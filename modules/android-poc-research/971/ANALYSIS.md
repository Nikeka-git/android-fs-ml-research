# 🔬 Глубокий анализ CVE-2026-20971 (PROCA/FIVE)

Этот документ содержит технический разбор уязвимости CVE-2026-20971 на уровне ядра, дизассемблирования и структур данных. В отличие от `README.md`, который описывает процесс тестирования, здесь представлен **академический разбор механики уязвимости** и причин её неэксплуатируемости на конкретном устройстве.

## 1. Восстановление `vmlinux` и подготовка к анализу

Для анализа ядра Samsung A51 (Linux 4.14.113) были выполнены следующие шаги:

1.  Из архива прошивки `AP_A515FXXU4EUC8_MULTI.zip` был извлечен `boot.img.lz4`.
2.  С помощью `lz4` и `unpack_bootimg` был получен несжатый `kernel.bin` (ARM64 Image).
3.  Утилита `vmlinux-to-elf` (версия 1.3.6) восстановила ELF-файл с базой `0xffffff8008080000` и **102 481 символом**.
4.  Восстановленный ELF был загружен в Ghidra для дизассемблирования и анализа.

**Важное замечание:** Восстановленный ELF не содержит отладочных секций (DWARF/BTF), поэтому структуры данных приходилось восстанавливать по дизассемблированию и релокациям.

## 2. Дизассемблирование ключевых функций

### 2.1. Функция `proc_integrity_label_read`
Эта функция вызывается при чтении файла `/proc/<pid>/integrity/label`.

```assembly
0xffffff80082a68dc:  ldr x8, [x20, #0xa70]    ; x8 = task->integrity
0xffffff80082a68e4:  add x0, x8, #0xc         ; x0 = &integrity->value_lock
0xffffff80082a68e8:  bl  _raw_spin_lock        ; spin_lock(&integrity->value_lock)
0xffffff80082a68ec:  ldr x8, [x20, #0xa70]    ; x8 = task->integrity (перечитали)
0xffffff80082a68f0:  ldr x21, [x8, #0x18]     ; x21 = integrity->label (указатель)
0xffffff80082a68f4:  add x0, x8, #0xc
0xffffff80082a68f8:  bl  _raw_spin_unlock
0xffffff80082a68fc:  cbz x21, #0xffffff80082a6940 ; if (!label) -> print -1
0xffffff80082a6900:  ldrh w22, [x21]          ; w22 = *label (чтение длины метки)
...
0xffffff80082a6978:  bl  bin2hex              ; копирование данных

Ключевой момент: Между ldr x21, [x8, #0x18] (получение указателя на label) и ldrh w22, [x21] (чтение длины) проходит несколько инструкций. Это и есть окно гонки. Если execve() освободит task_integrity (и его label) в этот момент, x21 станет висячим указателем (dangling pointer).

2.2. Функция task_integrity_free (и five_task_free)
Эта функция освобождает task_integrity при смерти процесса или execve().

assembly
0xffffff80083f6a10:  stp x29, x30, [sp, #-0x20]!
...
0xffffff80083f6a20:  cbz x19, #0xffffff80083f6a5c ; if (!integrity) return
0xffffff80083f6a24:  ldr x0, [x19, #0x18]         ; x0 = integrity->label
0xffffff80083f6a28:  bl  kfree                     ; kfree(label)
0xffffff80083f6a2c:  str xzr, [x19, #0x18]         ; integrity->label = NULL
...
0xffffff80083f6a4c:  adrp x8, #0xffffff800a117000 ; x8 = &task_integrity_cache
0xffffff80083f6a50:  ldr x0, [x8, #0x178]         ; x0 = cache_ptr
0xffffff80083f6a54:  mov x1, x19
0xffffff80083f6a58:  bl  kmem_cache_free           ; освобождение самой структуры
Ключевой момент: kfree(integrity->label) освобождает память, на которую указывает x21 в функции proc_integrity_label_read. Если чтение длины (ldrh w22, [x21]) произойдет после kfree, возникнет Use-After-Free.

3. Анализ структуры task_integrity
На основе дизассемблирования task_integrity_alloc и task_integrity_free была восстановлена структура task_integrity.

3.1. Размер структуры
В функции task_integrity_cache_init (VA 0xffffff800939d19c):

assembly
0xffffff800939d1b8:  mov w1, #0x60      ; размер = 96 байт
0xffffff800939d1bc:  movk w1, #0x140, lsl #16 ; flags = 0x1400000
...
0xffffff800939d1c4:  bl  kmem_cache_create
Вывод: struct task_integrity занимает 96 байт (0x60) и создается в выделенном кэше (task_integrity_cache).

3.2. Раскладка полей
Смещение	Поле	Тип	Размер
0x00	user_value	enum	4
0x04	value	enum	4
0x08	usage_count	atomic_t	4
0x0C	value_lock	spinlock_t	4
0x10	list_lock	spinlock_t	4
0x18	label	struct integrity_label *	8
0x20	events	struct processing_event_list	48
0x50	reset_cause	enum	4
0x58	reset_file	struct file *	8
Уязвимые поля:

label (0x18): освобождается через kfree, может стать dangling pointer.

reset_file (0x58): освобождается через fput.

4. Механизм гонки (Race Condition)
4.1. Сценарий UAF
Процесс A (жертва) имеет task_integrity с валидным label (например, хэш SHA256).

Поток чтения вызывает proc_integrity_label_read и получает указатель x21 = task->integrity->label.

Процесс A вызывает execve() (или умирает), и ядро вызывает task_integrity_free.

task_integrity_free выполняет kfree(integrity->label).

Поток чтения продолжает работу и обращается к x21 (уже освобожденная память).

4.2. Почему userfaultfd критичен
Для успешной эксплуатации необходимо заморозить поток чтения между шагами 2 и 5. Это позволяет:

Убедиться, что execve() завершился и память освобождена.

Заспреить освобожденную память контролируемым объектом (например, фейковым label с длиной 0xFFFF).

Разморозить поток, чтобы bin2hex прочитал наш фейковый объект.

На нашем устройстве userfaultfd отключен (CONFIG_USERFAULTFD is not set), поэтому мы не можем заморозить поток.

5. Анализ ограничений
5.1. Выделенный кэш (Dedicated Slab Cache)
task_integrity создается через kmem_cache_create с флагами SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_NOTRACK. Это означает, что структура находится в собственном кэше, а не в общем kmalloc.

Следствие: Стандартные спреи через setxattr или msg_msg не могут занять освобожденный слот. Требуется cross-cache атака: заставить ядро вернуть целую slab-страницу в buddy allocator, а затем заново выделить её под pipe_buffer или другой контролируемый объект.

5.2. SELinux и kill()
Для освобождения task_integrity нужно заставить процесс вызвать execve() или умереть. Мы пробовали:

am force-stop: слишком медленно, окно гонки закрывается.

kill(pid, SIGKILL): SELinux блокирует kill для системных процессов (EPERM).

5.3. timerfd (Hardirq) как альтернатива userfaultfd
Мы использовали timerfd для генерации аппаратных прерываний (hardirq) внутри критической секции.

Проблема: Прерывания не гарантируют вытеснение ровно между нужными инструкциями. В большинстве случаев прерывание происходит до или после окна. Это приводило либо к отсутствию UAF, либо к starvation (зависанию на спинлоках).

6. Сравнение с CVE-2021-0920
Параметр	CVE-2026-20971 (PROCA/FIVE)	CVE-2021-0920 (unix_gc)
Окно гонки	Наносекунды (между ldr и ldrh)	Микросекунды (между unix_detach_fds и unix_notinflight)
Спрей	Выделенный кэш (требуется cross-cache)	scm_fp_list (общий kmalloc)
Синхронизация	Нужен userfaultfd (отключен)	Можно использовать timerfd
Результат на A51	Провал (нестабильно)	Успех (Kernel Panic)
Вывод: CVE-2026-20971 намного сложнее для эксплуатации из-за выделенного кэша и микроскопического окна. CVE-2021-0920 предоставляет более широкие возможности для спрея и синхронизации.

7. Вывод
CVE-2026-20971 — это интересная уязвимость, демонстрирующая сложность современных механизмов безопасности Samsung (PROCA/FIVE). Однако на ядре 4.14.113 без userfaultfd и с жесткой политикой SELinux её эксплуатация не представляется возможной в рамках нашего исследовательского стенда. Дальнейшие попытки требуют либо отладочного ядра (с CONFIG_USERFAULTFD=y), либо обхода SELinux для прямого kill, либо сложной cross-cache атаки.

В рамках проекта этот эксплойт был классифицирован как «Неприменимый для ядра 4.14 без дополнительных уязвимостей».