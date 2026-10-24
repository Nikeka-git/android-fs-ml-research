<p align="center">
  <img src="docs/assets/research-banner.svg" alt="Android FS · ML Research — диагностика, данные и методика оценки" width="100%">
</p>

<h1 align="center">Android FS · ML Research</h1>

<p align="center">
  Исследование Android, файловых систем и методов машинного обучения.<br>
  Диагностический прототип, исследовательские материалы и воспроизводимая проверка данных.
</p>

<p align="center">
  <a href="docs/README.md"><b>Документация</b></a> ·
  <a href="modules/README.md"><b>Модули</b></a> ·
  <a href="data/README.md"><b>Данные</b></a> ·
  <a href="docs/team.md"><b>Команда и вклад</b></a> ·
  <a href="https://github.com/users/Nikeka-git/projects/1"><b>Доска проекта</b></a>
</p>

<p align="center">
  <a href="modules/README.md"><img src="https://img.shields.io/badge/модули-2-1e293b?style=flat-square&amp;labelColor=0f172a" alt="2 исследовательских модуля"></a>
  <a href="data/diagnostic-texts/manifest.json"><img src="https://img.shields.io/badge/публичные_тексты-12-0f766e?style=flat-square&amp;labelColor=0f172a" alt="12 публичных диагностических текстов"></a>
  <a href="evidence/imports/android-poc-research/FILE_INDEX.csv"><img src="https://img.shields.io/badge/исходные_тексты_импорта-204-4338ca?style=flat-square&amp;labelColor=0f172a" alt="204 исходных текста в исследовательском импорте"></a>
  <a href="https://github.com/Nikeka-git/android-fs-ml-research/tree/prob-period-1"><img src="https://img.shields.io/badge/версия-prob--period--1-475569?style=flat-square&amp;labelColor=0f172a" alt="Апробационная версия prob-period-1"></a>
</p>

## Начните здесь

| Хочу… | Куда перейти |
|---|---|
| **Понять тему и текущее состояние** | [Цель и границы](docs/project-scope.md) → [архитектура](docs/architecture.md) |
| **Посмотреть исходники** | [Каталог модулей](modules/README.md) → описание нужного модуля |
| **Изучить данные и методику** | [Подготовка данных](docs/research/data-preparation.md) → [методика оценки](docs/research/evaluation-methodology.md) |
| **Проверить индивидуальный вклад** | [Команда и подтверждения](docs/team.md) → [вклад Kerey404, PR №21](https://github.com/Nikeka-git/android-fs-ml-research/pull/21) |
| **Найти материалы для защиты** | [Навигатор по отчётам](docs/README.md#материалы-для-защиты) → [апробационная версия](docs/submission.md) |

## О проекте

Исследуем применение ML/AI к выбору существующих исследовательских материалов по характеристикам Android-устройства. Уточнённая цель проекта — автоматический выбор подходящего существующего эксплойта; текущий репозиторий содержит диагностический прототип и базу для дальнейшей оценки этого подхода.

**Руководитель:** Zhaksylyk Kozhakhmet · **Технический лидер:** Tsybus Nikita Valentinovich.

<details>
<summary><b>Официальная тема дипломной работы</b></summary>

Development of a Machine Learning-Based Exploit Generation Module for Various Android OS File Systems

Официальная тема сохранена. Уточнение цели и границы текущего этапа описаны в [спецификации исследования](docs/project-scope.md). Совместный Assignment 3 также описывает текстовый прототип и план будущего ACK-исследования; его заявленные результаты учитываются отдельно от новых проверок репозитория.

</details>

## Что уже подготовлено

| Направление | Что можно посмотреть | Подтверждённое состояние |
|---|---|---|
| **Диагностика Android** | [binder-diagnostics](modules/binder-diagnostics/README.md) · Kotlin / JNI / C++17 | Снимок прототипа: локальная симуляция и диагностика Binder |
| **Исследовательская база** | [android-poc-research](modules/android-poc-research/README.md) · [индекс файлов](evidence/imports/android-poc-research/FILE_INDEX.csv) | 204 сохранённых исходных текста; происхождение и SHA-256 зафиксированы |
| **Подготовка данных** | [12 публичных текстов](data/diagnostic-texts/manifest.json) · [правила подготовки](docs/research/data-preparation.md) | Проверены размеры и хеши 12/12 источников, всего 46 570 байт |
| **Методика оценки** | [Показатели, сравнение и ограничения](docs/research/evaluation-methodology.md) | Описаны метрики, разделение данных, парные сравнения и учёт неполных результатов |

> **Статус исследования.** Байтовая целостность источников проверена. Сборка и выполнение импортированных PoC не проверялись; новые Android/ACK-испытания не проводились. Результаты текстового ML-прототипа из Assignment 3 повторно не измерялись. Подробнее — в [реестре подтверждений](evidence/README.md).

## Команда и вклад

| Участник | Направление | Материалы и подтверждения |
|---|---|---|
| **Tsybus Nikita Valentinovich** · [Nikeka-git](https://github.com/Nikeka-git) | Техническое руководство, архитектура, прототип и исследование | [Диагностический снимок](https://github.com/Nikeka-git/android-fs-ml-research/commit/b23517afda69fdb3d9a937e5c2eb024060d95202) · [импорт исследовательской базы](https://github.com/Nikeka-git/android-fs-ml-research/commit/9b84849dcd6f092b61dc2edb638a46904016f62d) |
| **Kassymgali Bakytzhan Zhenisuly** · [Kerey404](https://github.com/Kerey404) | Подготовка исследовательских данных и методики оценки; оформление репозитория | [Данные и методика, PR #21](https://github.com/Nikeka-git/android-fs-ml-research/pull/21) · [навигация, PR #22](https://github.com/Nikeka-git/android-fs-ml-research/pull/22) · [задачи #23](https://github.com/Nikeka-git/android-fs-ml-research/issues/23), [#24](https://github.com/Nikeka-git/android-fs-ml-research/issues/24) |
| **Galidullaeva Aliya Khantanirovna** · [aliiyaaa](https://github.com/aliiyaaa) | Аналитическая документация и оформление результатов | [Аналитический PDF](docs/analytics.pdf) · [коммит добавления](https://github.com/Nikeka-git/android-fs-ml-research/commit/eb6dc9e2aa4f86ab6bc85cf1a6bca00171396714) · [задача #25](https://github.com/Nikeka-git/android-fs-ml-research/issues/25) |

Подробное описание фактического вклада, совместного авторства и открытых вопросов — в [сведениях о команде](docs/team.md). Наличие материала в репозитории само по себе не устанавливает индивидуальное авторство.

## Проверить данные

Из корня репозитория, **Python 3.12+**, без дополнительных пакетов:

```bash
# Проверить реестр и локальные исследовательские материалы
python tools/verify_research_sources.py

# Дополнительно сверить 12 публичных источников по HTTPS
python tools/verify_research_sources.py --fetch
```

Проверка сопоставляет размеры и SHA-256; входные тексты не выполняются. Это проверка происхождения материалов. Для Android-проекта используйте отдельное [описание модуля](modules/binder-diagnostics/README.md).

## Карта репозитория

```text
android-fs-ml-research/
├── modules/       Диагностический прототип и исследовательский снимок
├── docs/          Цель, архитектура, методика, команда и отчёты
├── data/          Реестры исследовательских данных
├── evidence/      Происхождение, хеши и сохранённые подтверждения
├── experiments/   Правила учёта будущих фактических запусков
├── tools/         Проверка исследовательских источников
├── planning/      План задач и соответствие GitHub Issues
└── shared/        Общие компоненты по мере появления
```

[Модули](modules/README.md) · [Документация](docs/README.md) · [Данные](data/README.md) · [Подтверждения](evidence/README.md) · [Архитектура подробнее](docs/architecture.md)

## Следующие шаги

- [x] Организовать монорепозиторий и сохранить происхождение импортированных материалов.
- [x] Подготовить реестр публичных диагностических текстов и проверку их целостности.
- [x] Оформить методику оценки и связать подготовленные материалы с вкладом участника.
- [ ] Независимо проверить разметку и воспроизвести заявленные результаты текстового прототипа.
- [ ] Подтвердить стенд и подготовить данные для целевого ACK-исследования.
- [ ] Выполнить согласованную оценку и обновить отчёт по фактическим результатам.

[Задачи GitHub](https://github.com/Nikeka-git/android-fs-ml-research/issues) · [Доска проекта](https://github.com/users/Nikeka-git/projects/1) · [Правила участия](CONTRIBUTING.md)

<details>
<summary><b>Версии, сохранённые отчёты и ограничения</b></summary>

- [`main`](https://github.com/Nikeka-git/android-fs-ml-research/tree/main) содержит текущие материалы.
- [`prob-period-1`](https://github.com/Nikeka-git/android-fs-ml-research/tree/prob-period-1) — версия сдачи, обновлённая 24 октября 2026 года с вкладом участников и актуальными материалами доски. Предыдущий снимок доступен по [коммиту df9da6c](https://github.com/Nikeka-git/android-fs-ml-research/commit/df9da6cbbc35fef1b0b9142b225e5ca6808fec5d).
- [Ответы на пункты 4–7](docs/prob-period-1.md) и [комплект сдачи](docs/submission.md) соответствуют обновлённой версии.
- [Текущий PDF доски и задач](docs/report/board-and-tasks-2026-10-24.pdf) датирован 24 октября. [Сохранённые общие отчёты](docs/report/README.md) и прежние снимки доски относятся к 15 октября 2026 года.
- [JUnit XML](evidence/stored/junit-unit-tests.xml) содержит 24 теста без ошибок; [lint-отчёт](evidence/stored/lint-results-debug.txt) — 0 ошибок и 1 предупреждение. Эти прежние проверки не перезапускались; JVM-тесты используют симуляцию.
- Исходники, модель и журналы текстового прототипа, описанного в Assignment 3, пока не предоставлены. Готовый обучающий набор и подтверждённое качество выбора эксплойта в репозитории не представлены.
- Прежнее авторство импортированных файлов, работоспособность PoC и охват разных устройств не устанавливаются фактом импорта. Сырые журналы, дампы, архивы и результаты сборки хранятся отдельно.

</details>
