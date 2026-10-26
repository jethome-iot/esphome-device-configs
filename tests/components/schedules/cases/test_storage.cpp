// The files: one per schedule under its id, what a boot loads and what it leaves alone, the
// cap, a newer firmware's file, writes and deletes that fail, names and ids.
#include "common.h"

namespace esphome::schedules::testing {

class Storage : public EngineTest {
 protected:
  void write(const std::string &file, const std::string &data) {
    mkdir(this->folder().c_str(), 0755);
    write_file(this->folder() + "/" + file, data);
  }
};

TEST_F(Storage, ACreateWritesTheFileUnderAnIdFromTheName) {
  this->boot();
  Result created = this->create(daily("Porch light"));
  EXPECT_EQ("porch-light", created.id);
  EXPECT_EQ(std::vector<std::string>{"porch-light.json"}, list_dir(this->folder()));
  EXPECT_EQ(this->engine->document("porch-light"), read_file(this->file_of("porch-light")));
  EXPECT_EQ("", this->engine->document("nobody"));
  EXPECT_EQ(SCHEDULE_VERSION, this->engine->get("porch-light")->version);
}

// The plan's example, sent as a draft: the file is that document byte for byte.
TEST_F(Storage, TheFileIsTheDocumentByteForByte) {
  const std::string golden = R"({"version":1,"id":"living-room-week","name":"Living room week","enabled":true,)"
                             R"("target":{"type":"thermostat","id":"living-room"},)"
                             R"("week":{"mon":[{"at":"06:30","value":{"type":"preset","preset":"comfort"}},)"
                             R"({"at":"08:30","value":{"type":"preset","preset":"eco"}},)"
                             R"({"at":"17:00","value":{"type":"target","target":21.5}},)"
                             R"({"at":"23:00","value":{"type":"off"}}],)"
                             R"("tue":[],"wed":[],"thu":[],"fri":[],"sat":[],"sun":[]}})";
  this->boot();
  ScheduleConfig draft;
  std::string error;
  ASSERT_TRUE(from_json(golden, &draft, &error)) << error;
  draft.id = "ignored";
  EXPECT_EQ("living-room-week", this->create(draft).id);
  EXPECT_EQ(golden, read_file(this->file_of("living-room-week")));
}

TEST_F(Storage, TheSchedulesComeBackAtTheNextBoot) {
  this->boot();
  this->create(daily("Porch"));
  this->create(daily("Garden", "relay_2", false));
  this->reboot();
  const std::vector<const ScheduleConfig *> all = this->engine->schedules();
  ASSERT_EQ(2u, all.size());
  EXPECT_EQ("garden", all[0]->id) << "sorted by id";
  EXPECT_FALSE(all[0]->enabled);
  EXPECT_EQ("porch", all[1]->id);
  EXPECT_EQ(14u, all[1]->points.size());
}

TEST_F(Storage, ARenameKeepsTheIdAndTheFile) {
  this->boot();
  this->create(daily("Porch"));
  ScheduleConfig renamed = daily("Front door");
  renamed.id = "elsewhere";
  Result saved = this->engine->update("porch", renamed);
  ASSERT_TRUE(saved.ok) << saved.error;
  EXPECT_EQ("porch", saved.id);
  EXPECT_EQ("Front door", this->engine->get("porch")->name);
  EXPECT_EQ(std::vector<std::string>{"porch.json"}, list_dir(this->folder())) << "no .tmp left either";
  EXPECT_NE(std::string::npos, read_file(this->file_of("porch")).find(R"("id":"porch","name":"Front door")"));
}

TEST_F(Storage, ADeleteRemovesTheFile) {
  this->boot();
  this->create(daily("Porch"));
  Result removed = this->engine->remove("porch");
  ASSERT_TRUE(removed.ok);
  EXPECT_TRUE(removed.persisted);
  EXPECT_EQ(nullptr, this->engine->get("porch"));
  EXPECT_TRUE(list_dir(this->folder()).empty());
  this->reboot();
  EXPECT_TRUE(this->engine->schedules().empty());
}

TEST_F(Storage, AFileThatCannotBeDeletedIsBlanked) {
  this->boot();
  this->create(daily("Porch"));
  this->engine->undeletable = {"porch.json"};
  Result removed = this->engine->remove("porch");
  ASSERT_TRUE(removed.ok);
  EXPECT_TRUE(removed.persisted);
  EXPECT_EQ("", read_file(this->file_of("porch")));
  EXPECT_TRUE(file_exists(this->file_of("porch")));
  this->reboot();
  EXPECT_TRUE(this->engine->schedules().empty()) << "the next boot refuses the empty file";
}

TEST_F(Storage, AFileThatCannotBeBlankedEitherComesBack) {
  this->boot();
  this->create(daily("Porch"));
  // In its place something no write can open: a folder.
  ::remove(this->file_of("porch").c_str());
  mkdir(this->file_of("porch").c_str(), 0755);
  this->engine->undeletable = {"porch.json"};
  Result removed = this->engine->remove("porch");
  ASSERT_TRUE(removed.ok);
  EXPECT_FALSE(removed.persisted);
  EXPECT_EQ(nullptr, this->engine->get("porch"));
  EXPECT_TRUE(LogCapture::instance().warned("it comes back at the next boot"));
}

TEST_F(Storage, ARefusedFileIsLeftExactlyAsItIs) {
  struct Case {
    const char *file;
    std::string data;
    const char *logged;
  };
  const Case cases[] = {
      {"bad.json", "{not json", "/bad.json' refused: not valid JSON"},
      {"big.json", std::string(MAX_FILE_BYTES + 1, ' '), "refused: 16385 bytes"},
      {"empty.json", "", "refused: 0 bytes"},
      {"other.json", file_doc("elsewhere", "Other"), "its id 'elsewhere' is not its file name"},
      {"new.json", file_doc("new", "New"), "the editor cannot open the id 'new'"},
      {"Upper.json", file_doc("Upper", "Upper"), "id must be a slug"},
      {"broken.json", R"({"id":"broken","name":"Broken","target":{"type":"relay","id":"relay_1"},"week":{"mon":[]}})",
       "/broken.json' refused: A schedule needs at least one switch point"},
  };
  for (const Case &c : cases)
    this->write(c.file, c.data);
  this->write("good.json", file_doc("good", "Good"));
  this->write("notes.txt", "not a schedule");
  // Named like one, but no file at all.
  mkdir((this->folder() + "/folder.json").c_str(), 0755);
  this->boot();
  ASSERT_EQ(1u, this->engine->schedules().size());
  EXPECT_EQ("good", this->engine->schedules()[0]->id);
  for (const Case &c : cases) {
    EXPECT_EQ(c.data, read_file(this->folder() + "/" + c.file)) << c.file;
    EXPECT_TRUE(LogCapture::instance().has(c.logged)) << c.logged;
  }
}

TEST_F(Storage, TheCapHoldsAtBootAndForANewOne) {
  for (const char *id : {"a", "b", "c", "d", "e"})
    this->write(std::string(id) + ".json", file_doc(id, id, id[0] == 'a' ? "relay_1" : "relay_2", false));
  this->boot();
  ASSERT_EQ(4u, this->engine->schedules().size());
  EXPECT_EQ(nullptr, this->engine->get("e"));
  EXPECT_TRUE(LogCapture::instance().warned("'e.json' left alone: at most 4 schedules"));

  const char *const FULL = "This device allows 4 schedules; delete one to add another";
  Result refused = this->engine->create(daily("Porch", "relay_1", false));
  EXPECT_EQ(507, refused.code);
  EXPECT_EQ(FULL, refused.error);
  ScheduleConfig back = daily("Porch", "relay_1", false);
  back.id = "porch";
  refused = this->engine->restore(back);
  EXPECT_EQ(507, refused.code);
  back.id = "a";
  back.name = "a";
  EXPECT_TRUE(this->engine->restore(back).ok) << "a replacement adds nothing";
}

// A rollback leaves files a newer firmware wrote: they run, but this one never writes them.
TEST_F(Storage, ANewerFirmwaresFileRunsButIsNeverWritten) {
  const std::string newer = file_doc("porch", "Porch", "relay_1", true, 2);
  this->write("porch.json", newer);
  this->boot(at_local(0, 9, 0));
  EXPECT_TRUE(this->e.relay1.state) << "it runs";
  EXPECT_TRUE(LogCapture::instance().warned("is version 2, from a newer firmware"));

  Result refused = this->engine->update("porch", daily("Porch"));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("A newer firmware wrote this schedule; update the firmware to change it", refused.error);
  ScheduleConfig back = daily("Porch");
  back.id = "porch";
  EXPECT_EQ(409, this->engine->restore(back).code);

  Result off = this->engine->set_enabled("porch", false);
  ASSERT_TRUE(off.ok);
  EXPECT_FALSE(off.persisted);
  EXPECT_EQ(ScheduleState::DISABLED, this->state("porch"));
  Result on = this->engine->set_enabled("porch", true);
  ASSERT_TRUE(on.ok);
  EXPECT_FALSE(on.persisted);
  EXPECT_EQ(newer, read_file(this->file_of("porch")));

  ASSERT_TRUE(this->engine->remove("porch").ok);
  EXPECT_FALSE(file_exists(this->file_of("porch")));
}

TEST_F(Storage, AWriteThatFailsChangesNothing) {
  this->boot(at_local(0, 9, 0));
  this->create(daily("Porch"));
  const std::string before = read_file(this->file_of("porch"));
  // Where the new file would be written first, something no write can open.
  mkdir((this->file_of("porch") + ".tmp").c_str(), 0755);
  Result refused = this->engine->update("porch", daily("Front door"));
  EXPECT_EQ(500, refused.code);
  EXPECT_EQ("The schedule's file could not be written", refused.error);
  EXPECT_FALSE(refused.persisted);
  EXPECT_EQ("Porch", this->engine->get("porch")->name);
  EXPECT_EQ(before, read_file(this->file_of("porch")));

  // Live, but back as it was after a reboot.
  Result off = this->engine->set_enabled("porch", false);
  ASSERT_TRUE(off.ok);
  EXPECT_FALSE(off.persisted);
  EXPECT_FALSE(this->engine->get("porch")->enabled);
  rmdir((this->file_of("porch") + ".tmp").c_str());
}

// Written, but the rename over the old file fails: nothing changes and no .tmp is left.
TEST_F(Storage, AFileThatCannotBeRenamedIntoPlaceChangesNothing) {
  this->boot();
  this->create(daily("Porch"));
  ::remove(this->file_of("porch").c_str());
  mkdir(this->file_of("porch").c_str(), 0755);
  write_file(this->file_of("porch") + "/keep", "x");
  Result refused = this->engine->update("porch", daily("Front door"));
  EXPECT_EQ(500, refused.code);
  EXPECT_EQ("Porch", this->engine->get("porch")->name);
  EXPECT_EQ(std::vector<std::string>{"porch.json"}, list_dir(this->folder()));
}

TEST_F(Storage, BootRefusesAnUnreadableFileAndLeavesIt) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root reads anywhere";
  this->write("locked.json", file_doc("locked", "Locked"));
  chmod(this->file_of("locked").c_str(), 0000);
  this->boot();
  chmod(this->file_of("locked").c_str(), 0644);
  EXPECT_TRUE(this->engine->schedules().empty());
  EXPECT_TRUE(LogCapture::instance().warned("Cannot open '" + this->file_of("locked") + "'"));
}

TEST_F(Storage, AFolderThatCannotBeReadFailsTheEngine) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root reads anywhere";
  mkdir(this->folder().c_str(), 0000);
  this->boot();
  chmod(this->folder().c_str(), 0755);
  EXPECT_TRUE(this->engine->is_failed());
  EXPECT_TRUE(LogCapture::instance().warned("Cannot read '" + this->folder() + "'"));
}

TEST_F(Storage, ACreateThatCannotBeWrittenAddsNothing) {
  this->boot();
  storage().path = "/proc/definitely-not-writable";
  Result refused = this->engine->create(daily("Porch"));
  storage().path = this->base_;
  EXPECT_EQ(500, refused.code);
  EXPECT_TRUE(this->engine->schedules().empty());
  EXPECT_TRUE(LogCapture::instance().warned("Cannot create"));
}

TEST_F(Storage, AFileOverTheCapIsNeverWritten) {
  this->boot();
  this->engine->max_file_bytes = 64;
  Result refused = this->engine->create(daily("Porch"));
  EXPECT_EQ(413, refused.code);
  EXPECT_EQ("The schedule's file would be over 16 KiB", refused.error);
  EXPECT_TRUE(refused.persisted) << "nothing changed, nothing lost";
  EXPECT_TRUE(list_dir(this->folder()).empty());
}

TEST_F(Storage, ADocumentCutShortByTheHeapIsNeverWritten) {
  struct Refusing : ArduinoJson::Allocator {
    void *allocate(size_t) override { return nullptr; }
    void deallocate(void *ptr) override { free(ptr); }
    void *reallocate(void *, size_t) override { return nullptr; }
  } refusing;
  this->boot();
  this->engine->json_allocator = &refusing;
  Result refused = this->engine->create(daily("Porch"));
  EXPECT_EQ(500, refused.code);
  EXPECT_TRUE(list_dir(this->folder()).empty());
}

TEST_F(Storage, TwoFilesUnderOneNameAreToldApartAtBoot) {
  this->engine->set_max_schedules(8);
  this->write("a.json", file_doc("a", "Porch", "relay_1", false));
  this->write("a2.json", file_doc("a2", "Porch 2", "relay_1", false));
  this->write("b.json", file_doc("b", "porch", "relay_2", false));
  const std::string long_name(48, 'x');
  this->write("c.json", file_doc("c", long_name.c_str(), "relay_1", false));
  this->write("d.json", file_doc("d", long_name.c_str(), "relay_2", false));
  this->boot();
  EXPECT_EQ("Porch", this->engine->get("a")->name);
  EXPECT_EQ("porch 3", this->engine->get("b")->name) << "\"porch 2\" is taken too";
  EXPECT_NE(std::string::npos, read_file(this->file_of("b")).find(R"("name":"porch 3")")) << "written back";
  EXPECT_EQ(std::string(46, 'x') + " 2", this->engine->get("d")->name) << "cut to stay within 48 characters";
}

TEST_F(Storage, ARenameAtBootOfANewerFileStaysInMemory) {
  this->write("a.json", file_doc("a", "Porch", "relay_1", false));
  this->write("b.json", file_doc("b", "Porch", "relay_2", false, 2));
  this->boot();
  EXPECT_EQ("Porch 2", this->engine->get("b")->name);
  EXPECT_TRUE(LogCapture::instance().warned("'b': the new name was not written"));
}

TEST_F(Storage, NamesAreUniqueAsAPersonReadsThem) {
  this->boot();
  this->create(daily("Porch light"));
  this->create(daily("Garden", "relay_2"));
  Result refused = this->engine->create(daily(" porch   LIGHT ", "relay_1", false));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("\"porch   LIGHT\" is already used by another schedule", refused.error);
  EXPECT_EQ(409, this->engine->update("garden", daily("PORCH LIGHT", "relay_2")).code);
  ScheduleConfig back = daily("Porch Light", "relay_1", false);
  back.id = "other";
  EXPECT_EQ(409, this->engine->restore(back).code);
  EXPECT_TRUE(this->engine->update("porch-light", daily("PORCH  light")).ok) << "its own name is no clash";
}

TEST_F(Storage, IdsSkipTakenAndRefusedOnesAndTheReservedOne) {
  this->write("porch.json", "{not json");
  this->boot();
  EXPECT_EQ("schedule", this->create(daily("Утро", "relay_1", false)).id);
  EXPECT_EQ("schedule-2", this->create(daily("Вечер", "relay_1", false)).id);
  EXPECT_EQ("porch-2", this->create(daily("Porch", "relay_1", false)).id) << "a refused file keeps its id";
  ASSERT_TRUE(this->engine->remove("schedule").ok);
  EXPECT_EQ("new-2", this->create(daily("New", "relay_1", false)).id) << "the editor's /schedules/new";
}

TEST_F(Storage, WhenEveryIdANameGivesIsTakenTheCreateIsRefused) {
  this->boot();
  for (unsigned n = 1; n <= 999; n++)
    write_file(this->file_of(id_with_suffix("x", n)), "");
  Result refused = this->engine->create(daily("X"));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("Every id made from \"X\" is taken by a file in the schedule folder; choose another name", refused.error);
}

TEST_F(Storage, ARestoreBringsAScheduleBackUnderItsId) {
  this->write("held.json", "{not json");
  this->boot(at_local(0, 9, 0));
  ScheduleConfig back = daily("Porch", "relay_1", false);
  back.id = "front-porch";
  Result restored = this->engine->restore(back);
  ASSERT_TRUE(restored.ok) << restored.error;
  EXPECT_EQ("front-porch", restored.id);
  EXPECT_FALSE(this->engine->get("front-porch")->enabled) << "as the backup had it";
  EXPECT_TRUE(this->e.relay1.writes.empty());

  back.enabled = true;
  back.name = "Front porch";
  restored = this->engine->restore(back);
  ASSERT_TRUE(restored.ok) << restored.error;
  EXPECT_EQ("Front porch", this->engine->get("front-porch")->name);
  EXPECT_EQ(std::vector<bool>{true}, this->e.relay1.writes) << "enabled, it applies the value in force";

  mkdir((this->file_of("front-porch") + ".tmp").c_str(), 0755);
  back.name = "Back porch";
  Result unwritten = this->engine->restore(back);
  rmdir((this->file_of("front-porch") + ".tmp").c_str());
  EXPECT_EQ(500, unwritten.code);
  EXPECT_EQ("Front porch", this->engine->get("front-porch")->name);

  struct Case {
    const char *id;
    uint16_t code;
    const char *error;
  };
  for (const Case &c : {
           Case{"", 400, "id is required"},
           Case{"Porch", 400, "id must be a slug: lowercase letters, digits and single dashes"},
           Case{"new", 400, "id 'new' is reserved"},
           Case{"held", 409, "The id \"held\" is taken by a file in the schedule folder that was not loaded"},
       }) {
    ScheduleConfig other = daily("Other", "relay_2", false);
    other.id = c.id;
    Result refused = this->engine->restore(other);
    EXPECT_EQ(c.code, refused.code) << c.id;
    EXPECT_EQ(c.error, refused.error) << c.id;
  }
}

TEST_F(Storage, ABrokenRuleIsA400WhereverItComesIn) {
  this->boot();
  this->create(daily("Porch"));
  ScheduleConfig broken = relay_doc("Porch", "relay_1", {preset_at(0, "eco")});
  EXPECT_EQ(400, this->engine->create(broken).code);
  EXPECT_EQ(400, this->engine->update("porch", broken).code);
  broken.id = "porch";
  Result refused = this->engine->restore(broken);
  EXPECT_EQ(400, refused.code);
  EXPECT_EQ("Mon 00:00: a relay takes on or off", refused.error);
  EXPECT_EQ(400, this->engine->create(relay_doc("  ", "relay_1", {on_at(0)})).code);
}

TEST_F(Storage, AnUnknownIdIs404) {
  this->boot();
  for (const Result &result : {this->engine->update("nobody", daily()), this->engine->remove("nobody"),
                               this->engine->set_enabled("nobody", true)}) {
    EXPECT_EQ(404, result.code);
    EXPECT_EQ("Schedule not found", result.error);
  }
  EXPECT_EQ(ScheduleState::DISABLED, this->state("nobody"));
}

TEST_F(Storage, WithoutItsStorageTheEngineRefusesEveryChange) {
  storage().mounted = false;
  this->boot();
  EXPECT_TRUE(this->engine->is_failed());
  EXPECT_TRUE(LogCapture::instance().warned("Storage not mounted"));
  ScheduleConfig back = daily();
  back.id = "porch";
  for (const Result &result :
       {this->engine->create(daily()), this->engine->update("porch", daily()), this->engine->restore(back),
        this->engine->remove("porch"), this->engine->set_enabled("porch", true)}) {
    EXPECT_EQ(500, result.code);
    EXPECT_EQ("Schedule storage is not available", result.error);
  }
  // A job runs where it stands: a failed component is never scheduled.
  bool ran = false;
  EXPECT_TRUE(this->engine->run_on_loop([&ran]() { return ran = true; }));
  EXPECT_TRUE(ran);
}

TEST_F(Storage, AFolderThatCannotBeMadeFailsTheEngine) {
  storage().path = "/proc/definitely-not-writable";
  this->boot();
  storage().path = this->base_;
  EXPECT_TRUE(this->engine->is_failed());
  EXPECT_TRUE(LogCapture::instance().warned("Cannot create '/proc/definitely-not-writable/schedules'"));
}

TEST_F(Storage, AJobFromTheLoopRunsInPlace) {
  this->boot();
  int ran = 0;
  EXPECT_TRUE(this->engine->run_on_loop([&ran]() {
    ran++;
    return true;
  }));
  EXPECT_EQ(1, ran);
  EXPECT_EQ(4, this->engine->max_schedules());
}

TEST_F(Storage, TheConfigDumpSaysWhereEachOneStands) {
  this->write("old.json", file_doc("old", "Old", "relay_2", true, 2));
  this->boot(at_local(0, 9, 0));
  this->create(daily("Porch"));
  this->create(daily("Attic", "relay_7"));
  LogCapture::instance().clear();
  this->engine->dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Schedules: 3 of 4"));
  EXPECT_TRUE(LogCapture::instance().has("'Porch' (porch): relay 'relay_1', running"));
  EXPECT_TRUE(LogCapture::instance().has(
      "'Attic' (attic): relay 'relay_7', missing_target: Relay \"relay_7\" is not on this device"));
  EXPECT_TRUE(LogCapture::instance().has("'Old' (old): relay 'relay_2', running, file from a newer firmware"));
}

}  // namespace esphome::schedules::testing
