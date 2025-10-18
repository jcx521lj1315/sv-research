#include <QMessageBox>
#include <QStandardItemModel>
#include <QFileDialog>
#include <algorithm>
#include "RaidCalc.h"
#include "PokemonNames.h"
#include "Benchmarks.h"
#include "FormUtils.h"

RaidCalc::RaidCalc(QWidget* parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    if (RAIDCALC_VERSION[0] != '\0')
        setWindowTitle(windowTitle() + " " + RAIDCALC_VERSION);
    if (!SeedFinder::initialize())
    {
        QMessageBox::critical(this, "Error", "Failed to initialize seed finder.");
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
    ui.comboBoxEvent->addItem("None");
    for (auto event_name : event_names)
        ui.comboBoxEvent->addItem(event_name);
    std::set<uint32_t> encounterables;
    auto visitor = [&](const EncounterTera9& enc, Map map) { encounterables.insert(enc.species); };
    SeedFinder::visit_encounters(-1, visitor);
    create_species_filters(encounterables, species_filters[0]);
    for (int32_t i = 0; i < _countof(event_names); ++i)
    {
        SeedFinder::visit_encounters(i, visitor);
        create_species_filters(encounterables, species_filters[i + 1]);
    }
    add_options(ui.comboBoxSpecies, species_filters[0]);
    add_sorted_options(ui.comboBoxTeraType, type_names, _countof(type_names));
    add_sorted_options(ui.comboBoxAbility, ability_names + 1, _countof(ability_names) - 1);
    add_sorted_options(ui.comboBoxNature, nature_names, _countof(nature_names));
    ui.tableSeeds->setModel(&seedModel);
    itemFilters = new ItemFilterDialog(this);
    seedViewer = new SeedViewerDialog(this);
    settings = new SettingsDialog(this, ui.tableSeeds);
    about = new AboutDialog(this);
    encounterDb = new EncounterDatabaseDialog(this);
    exportOptions = new ExportDialog(this, &seedModel);
    finder_timer = new QTimer(this);
    connect(finder_timer, &QTimer::timeout, this, &RaidCalc::on_finder_timer_timeout);
    connect(ui.actionExit, &QAction::triggered, qApp, &QCoreApplication::quit);
    connect(encounterDb, &EncounterDatabaseDialog::parameterChangeRequested, this, &RaidCalc::on_parameterChangeRequested);
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    for (uint32_t i = 0; i < sys_info.dwNumberOfProcessors - 1; ++i)
        ui.comboBoxThreads->addItem(QString::number(i + 1));
    ui.comboBoxThreads->setCurrentIndex(ui.comboBoxThreads->count() - 1);
    set_event_group_visible(false);

    min_iv_widgets[0] = ui.spinBoxMinHP;
    min_iv_widgets[1] = ui.spinBoxMinAtk;
    min_iv_widgets[2] = ui.spinBoxMinDef;
    min_iv_widgets[3] = ui.spinBoxMinSpA;
    min_iv_widgets[4] = ui.spinBoxMinSpD;
    min_iv_widgets[5] = ui.spinBoxMinSpe;
    max_iv_widgets[0] = ui.spinBoxMaxHP;
    max_iv_widgets[1] = ui.spinBoxMaxAtk;
    max_iv_widgets[2] = ui.spinBoxMaxDef;
    max_iv_widgets[3] = ui.spinBoxMaxSpA;
    max_iv_widgets[4] = ui.spinBoxMaxSpD;
    max_iv_widgets[5] = ui.spinBoxMaxSpe;
    widgets_might = {
        ui.labelHeight,
        ui.spinBoxMinHeight,
        ui.labelSep7,
        ui.spinBoxMaxHeight,
        ui.labelWeight,
        ui.spinBoxMinWeight,
        ui.labelSep8,
        ui.spinBoxMaxWeight,
        ui.labelScale,
        ui.spinBoxMinScale,
        ui.labelSep9,
        ui.spinBoxMaxScale,
    };

    do_benchmarks(finder);
}

RaidCalc::~RaidCalc()
{

}

void RaidCalc::create_species_filters(std::set<uint32_t>& encounterables, std::vector<std::pair<std::string, uint32_t>>& filters)
{
    for (auto& species : encounterables)
    {
        SpeciesFilter filter;
        filter.value = 0;
        filter.species = species;
        filter.any_form = 1;
        if (FormUtils::has_rare_form(species))
        {
            filters.push_back({ std::string(pokemon_names[species]) + " (Any)", filter.value });
            filter.any_form = 0;
            filter.common_form = 1;
            filters.push_back({ std::string(pokemon_names[species]) + " (Common)", filter.value });
            filter.common_form = 0;
            filter.rare_form = 1;
            filters.push_back({ std::string(pokemon_names[species]) + " (Rare)", filter.value });
            continue;
        }
        auto forms = FormUtils::get_forms(species);
        if (forms.empty())
        {
            filters.push_back({ pokemon_names[species], filter.value });
            continue;
        }
        if (forms.size() > 1)
            filters.push_back({ std::string(pokemon_names[species]) + " (Any)", filter.value});
        filter.any_form = 0;
        for (auto form : forms)
        {
            filter.form = form;
            filters.push_back({ FormUtils::get_pokemon_name(species, form), filter.value });
        }
    }
    std::sort(filters.begin(), filters.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    encounterables.clear();
}

void RaidCalc::add_sorted_options(QComboBox* combo, const char** names, uint32_t name_count, uint32_t offset)
{
    std::vector<std::pair<std::string, uint32_t>> options;
    for (uint32_t i = 0; i < name_count; ++i)
        options.push_back({ names[i], i + offset });
    add_sorted_options(combo, options);
}

void RaidCalc::add_sorted_options(QComboBox* combo, std::vector<std::pair<std::string, uint32_t>>& options)
{
    std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    add_options(combo, options);
}

void RaidCalc::add_options(QComboBox* combo, std::vector<std::pair<std::string, uint32_t>>& options)
{
    if (options.empty())
        return;
    combo->addItem("Any", 0U);
    for (auto& pair : options)
        combo->addItem(pair.first.c_str(), pair.second);
}

void RaidCalc::toggle_ui(bool enabled)
{
    for (auto widget : ui.basicGroup->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        widget->setEnabled(enabled);
    bool is7 = ui.comboBoxStars->currentIndex() == 6;
    for (auto widget : ui.pokemonGroup->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        widget->setEnabled(enabled && (widgets_might.find(widget) != widgets_might.end() || !is7));
    for (auto widget : ui.itemGroup->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        widget->setEnabled(enabled);
    ui.tableSeeds->setEnabled(enabled);
    ui.menuBar->setEnabled(enabled);
    if (!enabled)
    {
        itemFilters->hide();
        seedViewer->hide();
        encounterDb->hide();
        seedModel.clear();
    }
}

void RaidCalc::on_buttonFindSeeds_clicked()
{
    finder.game = (Game)ui.comboBoxGame->currentIndex();
    finder.map_id = ui.comboBoxMap->currentIndex();
    finder.event_id = ui.comboBoxEvent->currentIndex() - 1;
    finder.event_group = ui.comboBoxEventGroup->currentData().toUInt();
    finder.stars = ui.comboBoxStars->currentIndex() + 1;
    finder.stage = ui.comboBoxStage->currentIndex();
    finder.raid_boost = ui.spinBoxRaidBoost->value();
    finder.thread_count = ui.comboBoxThreads->currentIndex() + 1;
    if (!hex_to_uint32(ui.editMinSeed->text(), finder.min_seed))
    {
        QMessageBox::critical(this, "Error", "Bad min seed value.");
        return;
    }
    if (!hex_to_uint32(ui.editMaxSeed->text(), finder.max_seed))
    {
        QMessageBox::critical(this, "Error", "Bad max seed value.");
        return;
    }
    if (finder.max_seed < finder.min_seed)
    {
        QMessageBox::critical(this, "Error", "Max seed cannot be smaller than min seed.");
        return;
    }
    if (finder.stars == 7 && !SeedFinder::is_mighty_event(finder.event_id))
    {
        QMessageBox::critical(this, "Error", "This event doesn't have any 7* raids.");
        return;
    }
    SpeciesFilter filter;
    filter.value = ui.comboBoxSpecies->currentData().toUInt();
    finder.species = filter.species;
    finder.form = filter.form;
    if (filter.any_form)
        finder.form = SeedFinder::AnyForm;
    else if (filter.rare_form)
        finder.form = SeedFinder::RareForm;
    else if (filter.common_form)
        finder.form = SeedFinder::CommonForm;
    finder.shiny = ui.comboBoxShiny->currentIndex();
    finder.tera_type = ui.comboBoxTeraType->currentData().toUInt();
    finder.ability = ui.comboBoxAbility->currentData().toUInt();
    finder.nature = ui.comboBoxNature->currentData().toUInt();
    finder.gender = ui.comboBoxGender->currentIndex();
    for (size_t i = 0; i < _countof(min_iv_widgets); ++i)
    {
        finder.min_iv[i] = min_iv_widgets[i]->value();
        finder.max_iv[i] = max_iv_widgets[i]->value();
        if (finder.min_iv[i] > finder.max_iv[i])
        {
            QMessageBox::critical(this, "Error", "Min IV value must be smaller than max IV value.");
            return;
        }
    }
    finder.min_height = ui.spinBoxMinHeight->value();
    finder.max_height = ui.spinBoxMaxHeight->value();
    finder.min_weight = ui.spinBoxMinWeight->value();
    finder.max_weight = ui.spinBoxMaxWeight->value();
    finder.min_scale = ui.spinBoxMinScale->value();
    finder.max_scale = ui.spinBoxMaxScale->value();
    if (finder.min_height > finder.max_height || finder.min_weight > finder.max_weight || finder.min_scale > finder.max_scale)
    {
        QMessageBox::critical(this, "Error", "Min value must be smaller than max value.");
        return;
    }
    finder.item_filters_active = ui.checkboxItemFilters->isChecked();
    finder.drop_threshold = ui.spinBoxMinItemsSum->value();
    itemFilters->update_seed_finder(finder);
    if (!finder.use_filters())
    {
        uint64_t seed_count = finder.max_seed - finder.min_seed + 1ULL;
        if (seed_count > MaxSeeds)
        {
            QMessageBox::critical(this, "Error", QString("No filters set. Search aborted because it would exceed maximum allowed number of results (%1).").arg(MaxSeeds));
            return;
        }
        if (seed_count > SeedCountWarningThreshold)
        {
            auto result = QMessageBox::question(this, "Warning", "No filters set, this may result in a huge result set. Do you want to continue anyway?");
            if (result != QMessageBox::Yes)
                return;
        }
    }
    toggle_ui(false);
    if (!finder.find_seeds())
    {
        QMessageBox::critical(this, "Error", "Failed to start seed finder.");
        toggle_ui(true);
        return;
    }
    finder_timer->start(250);
}

void RaidCalc::on_buttonEditFilters_clicked()
{
    itemFilters->show();
}

void RaidCalc::on_buttonResetPokemonFilters_clicked()
{
    ui.comboBoxSpecies->setCurrentIndex(0);
    ui.comboBoxShiny->setCurrentIndex(0);
    ui.comboBoxTeraType->setCurrentIndex(0);
    ui.comboBoxAbility->setCurrentIndex(0);
    ui.comboBoxNature->setCurrentIndex(0);
    ui.comboBoxGender->setCurrentIndex(0);
    for (auto& widget : min_iv_widgets)
        widget->setValue(0);
    for (auto& widget : max_iv_widgets)
        widget->setValue(31);
    ui.spinBoxMinHeight->setValue(0);
    ui.spinBoxMinWeight->setValue(0);
    ui.spinBoxMinScale->setValue(0);
    ui.spinBoxMaxHeight->setValue(255);
    ui.spinBoxMaxWeight->setValue(255);
    ui.spinBoxMaxScale->setValue(255);
}

void RaidCalc::on_buttonMaxIV_clicked()
{
    for (auto& widget : min_iv_widgets)
        widget->setValue(31);
    for (auto& widget : max_iv_widgets)
        widget->setValue(31);
}

void RaidCalc::on_finder_timer_timeout()
{
    if (!finder.is_search_done())
        return;
    finder_timer->stop();
    QString msg = QString("Found %1 seeds in %2ms.").arg(QString::number(finder.seeds.size()), QString::number(finder.time_taken.milliseconds()));
    QMessageBox::information(this, "Success", msg);
    seedModel.populateModel(finder);
    resultParams = finder.get_basic_params();
    toggle_ui(true);
    ui.actionExportSeeds->setEnabled(!finder.seeds.empty());
}

void RaidCalc::on_actionExportSeeds_triggered(bool checked)
{
    exportOptions->set_params(resultParams);
    exportOptions->open();
}

void RaidCalc::on_actionSeedViewer_triggered(bool checked)
{
    seedViewer->show();
}

void RaidCalc::on_actionSettings_triggered(bool checked)
{
    settings->open();
}

void RaidCalc::on_actionAbout_triggered(bool checked)
{
    about->open();
}

void RaidCalc::on_actionEncounterDatabase_triggered(bool checked)
{
    encounterDb->show();
}

void RaidCalc::on_tableSeeds_doubleClicked(const QModelIndex& index)
{
    seedViewer->display_seed(resultParams, seedModel.get_seed(index.row()));
    seedViewer->show();
}

const RaidCalc::StarsRange& RaidCalc::get_allowed_stars(int progress)
{
    static const StarsRange allowed_stars_story[5] =
    {
        { 1, 2 },
        { 1, 3 },
        { 1, 4 },
        { 3, 5 },
        { 3, 6 },
    };
    static const StarsRange allowed_stars_event[4] =
    {
        { 1, 2 },
        { 1, 3 },
        { 1, 4 },
        { 3, 7 },
    };
    return ui.comboBoxEvent->currentIndex() == 0 ? allowed_stars_story[progress] : allowed_stars_event[progress];
}

void RaidCalc::on_comboBoxEvent_currentIndexChanged(int index)
{
    ui.comboBoxStage->blockSignals(true);
    ui.comboBoxStage->clear();
    ui.comboBoxEventGroup->clear();
    if (index == 0)
    {
        ui.labelMap->setVisible(true);
        ui.comboBoxMap->setVisible(true);
        ui.labelStage->setText("Story progress:");
        for (auto& stage_name : stage_names_story)
            ui.comboBoxStage->addItem(stage_name);
        set_event_group_visible(false);
    }
    else
    {
        ui.labelMap->setVisible(false);
        ui.comboBoxMap->setVisible(false);
        ui.labelStage->setText("Event progress:");
        for (auto& stage_name : stage_names_event)
            ui.comboBoxStage->addItem(stage_name);
        auto group = SeedFinder::get_event_info(index - 1);
        for (auto id : group->dist)
            ui.comboBoxEventGroup->addItem(QString::number(id), id);
        set_event_group_visible(ui.comboBoxEventGroup->count() > 1);
    }
    ui.comboBoxStage->setCurrentIndex(ui.comboBoxStage->count() - 1);
    ui.comboBoxStage->blockSignals(false);
    ui.comboBoxStars->blockSignals(true);
    ui.comboBoxStars->setCurrentIndex(5);
    ui.comboBoxStars->blockSignals(false);
    ui.comboBoxSpecies->clear();
    add_options(ui.comboBoxSpecies, species_filters[index]);
    ui.comboBoxSpecies->setCurrentIndex(0);
    for (auto widget : ui.pokemonGroup->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        widget->setEnabled(true);
}

void RaidCalc::on_comboBoxStage_currentIndexChanged(int index)
{
    auto& range = get_allowed_stars(index);
    int stars = ui.comboBoxStars->currentIndex() + 1;
    if (stars < range.min_stars)
    {
        ui.comboBoxStars->setCurrentIndex(range.min_stars - 1);
        QApplication::beep();
    }
    if (stars > range.max_stars)
    {
        ui.comboBoxStars->setCurrentIndex(range.max_stars - 1);
        QApplication::beep();
    }
}

void RaidCalc::on_comboBoxStars_currentIndexChanged(int index)
{
    fix_progress(index + 1);
    int event_id = ui.comboBoxEvent->currentIndex() - 1;
    int stars = ui.comboBoxStars->currentIndex() + 1;
    bool is7 = stars == 7;
    if (is7)
    {
        set_event_group_visible(false);
        if (SeedFinder::is_mighty_event(event_id))
        {
            set_mighty_parameters();
        }
        else
        {
            on_buttonResetPokemonFilters_clicked();
        }
    }
    else
    {
        set_event_group_visible(ui.comboBoxEventGroup->count() > 1);
    }
    for (auto widget : ui.pokemonGroup->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
        widget->setEnabled(widgets_might.find(widget) != widgets_might.end() || !is7);
}

void RaidCalc::on_comboBoxGame_currentIndexChanged(int index)
{
    if (SeedFinder::is_mighty_event(ui.comboBoxEvent->currentIndex() - 1))
    {
        set_mighty_parameters();
    }
}

void RaidCalc::fix_progress(int stars)
{
    int progress = ui.comboBoxStage->currentIndex();
    auto& range = get_allowed_stars(progress);
    if (stars >= range.min_stars && stars <= range.max_stars)
        return;
    bool fixed_progress = false;
    for (int i = 0; i < 5; ++i)
    {
        auto& candidate = get_allowed_stars(i);
        if (stars >= candidate.min_stars && stars <= candidate.max_stars)
        {
            ui.comboBoxStage->setCurrentIndex(i);
            fixed_progress = true;
            break;
        }
    }
    if (!fixed_progress)
    {
        ui.comboBoxStars->blockSignals(true);
        ui.comboBoxStars->setCurrentIndex(range.max_stars - 1);
        ui.comboBoxStars->blockSignals(false);
    }
    QApplication::beep();
}

void RaidCalc::set_event_group_visible(bool visibility)
{
    ui.labelEventGroup->setVisible(visibility);
    ui.comboBoxEventGroup->setVisible(visibility);
}

void RaidCalc::on_parameterChangeRequested(EncounterEntry entry, int32_t species)
{
    ui.comboBoxGame->setCurrentIndex((int)entry.game);
    ui.comboBoxEvent->setCurrentIndex(entry.event_id < 0 ? 0 : entry.event_id + 1);
    if (ui.comboBoxEventGroup->isVisible())
        select_option(ui.comboBoxEventGroup, entry.event_group);
    ui.comboBoxStars->setCurrentIndex(entry.stars - 1);
    if (entry.stars != 7)
    {
        SpeciesFilter filter;
        filter.value = 0;
        filter.species = species;
        filter.any_form = 1;
        select_option(ui.comboBoxSpecies, filter.value);
    }
}

void RaidCalc::set_mighty_parameters()
{
    int event_id = ui.comboBoxEvent->currentIndex() - 1;
    finder.game = (Game)ui.comboBoxGame->currentIndex();
    finder.map_id = ui.comboBoxMap->currentIndex();
    finder.event_id = event_id;
    finder.event_group = SeedFinder::get_event_info(event_id)->might.front();
    finder.stars = ui.comboBoxStars->currentIndex() + 1;
    finder.stage = ui.comboBoxStage->currentIndex();
    finder.raid_boost = ui.spinBoxRaidBoost->value();
    SeedFinder::SeedInfo info = finder.get_seed_info(0);
    SpeciesFilter filter;
    filter.value = 0;
    filter.species = info.species;
    if (FormUtils::get_forms(info.species).empty())
        filter.any_form = 1;
    else
        filter.form = info.form;
    select_option(ui.comboBoxSpecies, filter.value);
    select_option(ui.comboBoxTeraType, info.tera_type + 1);
    select_option(ui.comboBoxAbility, info.ability);
    select_option(ui.comboBoxNature, info.nature + 1);
    ui.comboBoxShiny->setCurrentIndex(info.shiny ? 1 : 2);
    ui.comboBoxGender->setCurrentIndex(info.gender + 1);
    for (size_t i = 0; i < _countof(min_iv_widgets); ++i)
        min_iv_widgets[i]->setValue(info.iv[i]);
    for (size_t i = 0; i < _countof(max_iv_widgets); ++i)
        max_iv_widgets[i]->setValue(info.iv[i]);
    ui.spinBoxMinHeight->setValue(0);
    ui.spinBoxMinWeight->setValue(0);
    ui.spinBoxMinScale->setValue(0);
    ui.spinBoxMaxHeight->setValue(255);
    ui.spinBoxMaxWeight->setValue(255);
    ui.spinBoxMaxScale->setValue(255);
}
