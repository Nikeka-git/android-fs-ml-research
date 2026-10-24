[Главная](../README.md) / [Документация](README.md) / **Команда и вклад**

# Команда и вклад

ФИО приведены латиницей по сведениям автора. Вклад сверен с историей репозитория **24 октября 2026 года**. Руководитель: **Zhaksylyk Kozhakhmet**.

| Участник | GitHub | Область ответственности |
|---|---|---|
| Tsybus Nikita Valentinovich | [Nikeka-git](https://github.com/Nikeka-git) | Технический лидер: архитектура, реализация прототипа, основная исследовательская работа и организация материалов |
| Kassymgali Bakytzhan Zhenisuly | [Kerey404](https://github.com/Kerey404) | Подготовка исследовательских данных и методики оценки; оформление репозитория и навигации |
| Galidullaeva Aliya Khantanirovna | [aliiyaaa](https://github.com/aliiyaaa) | Аналитическая документация и оформление результатов |

## Завершённые материалы и подтверждения

**Tsybus Nikita Valentinovich.** Технический лидер. История репозитория подтверждает подготовку монорепозитория, перенос диагностического проекта и сохранённых отчётов в [b23517a](https://github.com/Nikeka-git/android-fs-ml-research/commit/b23517afda69fdb3d9a937e5c2eb024060d95202), организацию единого исследовательского снимка и индекса 204 исходных текстов в [9b84849](https://github.com/Nikeka-git/android-fs-ml-research/commit/9b84849dcd6f092b61dc2edb638a46904016f62d), подготовку апробационной документации в [df9da6c](https://github.com/Nikeka-git/android-fs-ml-research/commit/df9da6cbbc35fef1b0b9142b225e5ca6808fec5d) и уточнение ролей команды в [de22fe5](https://github.com/Nikeka-git/android-fs-ml-research/commit/de22fe59b66c00b5b25f58ace22e7052d4089e21). Завершённые задачи: [#8](https://github.com/Nikeka-git/android-fs-ml-research/issues/8), [R01–R08 / #13–#20](https://github.com/Nikeka-git/android-fs-ml-research/issues?q=is%3Aissue+is%3Aclosed+assignee%3ANikeka-git). Импорт подтверждает сохранение материалов; первоначальное авторство каждого импортированного файла и новые успешные испытания им не устанавливаются.

**Kassymgali Bakytzhan Zhenisuly.** 24 октября 2026 года подготовлен пакет исследовательских данных и методики оценки:

- [Описание подготовки данных](research/data-preparation.md): происхождение источников, словарь полей, правила разметки, обработки пропусков, дубликатов и разделения без утечки данных.
- [Реестр 12 публичных диагностических текстов](../data/diagnostic-texts/manifest.json): закреплённые URL, размеры и SHA-256. При подготовке получены 12/12 файлов общим размером 46 570 байт; разметка перенесена из совместного отчёта и ожидает независимой проверки.
- [Проверка происхождения](../tools/verify_research_sources.py): локальная проверка материалов и дополнительная загрузка закреплённых публичных источников с проверкой байтов.
- [Методика оценки](research/evaluation-methodology.md): показатели парсера и текстового ML-компонента, базы сравнения, единицы наблюдения, парные сравнения, учёт неполных данных и ограничения будущего ACK-исследования.
- [Совместный Assignment 3](research/sources/assignment3-tsybus-kassymgali.docx), схема и [манифест получения](../evidence/methodology/assignment3-manifest.json) с сохранением исходного авторства Никиты Цыбуса и Бакытжана Касымгали.

Индивидуальный результат — подготовка и оформление указанного пакета в [60c5ca2](https://github.com/Nikeka-git/android-fs-ml-research/commit/60c5ca231a5c749a7ef707516d5fa3fcf44b0bca), включённого через [PR #21](https://github.com/Nikeka-git/android-fs-ml-research/pull/21). Отдельно оформлены README, каталог документации, данных и модулей, реестр подтверждений и навигация: [4f049e5](https://github.com/Nikeka-git/android-fs-ml-research/commit/4f049e55f245c0a71148a4dee959e52fb96ed8f3), [70a027b](https://github.com/Nikeka-git/android-fs-ml-research/commit/70a027bdaf733978e84db1727a47b9826a996e19), [PR #22](https://github.com/Nikeka-git/android-fs-ml-research/pull/22). Завершённые задачи: [R09 / #23](https://github.com/Nikeka-git/android-fs-ml-research/issues/23), [R10 / #24](https://github.com/Nikeka-git/android-fs-ml-research/issues/24).

Подготовка пакета не устанавливает единоличное авторство более ранних программ совместного отчёта. Обучение модели, повторный запуск описанных тестов, сборка Android и испытания ядра этим вкладом не заявляются.

**Galidullaeva Aliya Khantanirovna.** Добавила [аналитический PDF](analytics.pdf) с описанием цели, двух модулей, текущего диагностического прототипа, сохранённых результатов и ограничений: [eb6dc9e](https://github.com/Nikeka-git/android-fs-ml-research/commit/eb6dc9e2aa4f86ab6bc85cf1a6bca00171396714), завершённая задача [R11 / #25](https://github.com/Nikeka-git/android-fs-ml-research/issues/25). История подтверждает загрузку документа аккаунтом aliiyaaa. Документ представляет аналитическое описание материалов и не подтверждает новые эксперименты или качество готовой ML-модели.

## Задачи и ревью

YouTrack не используется; ID выше относятся к GitHub Issues. Соответствие локального плана опубликованным задачам находится в [planning/published-issues.json](../planning/published-issues.json). Ретроспективные задачи регистрируют существующие результаты: дата их создания не выдаётся за дату разработки.

Пакет Kerey404 связан также с [P07 / #7](https://github.com/Nikeka-git/android-fs-ml-research/issues/7). Общая методика помогает планированию P05/P06, но не закрывает требования к конкретному стенду `binder-diagnostics`. PR #21 и #22 подтверждают включение изменений; само слияние не подтверждает проведение независимого ревью. Подтверждённые отзывы и возникшие по ним правки отдельно не заявляются.

[← Документация](README.md) · [Подготовка данных](research/data-preparation.md) · [Методика оценки](research/evaluation-methodology.md) · [Подтверждения](../evidence/README.md)
