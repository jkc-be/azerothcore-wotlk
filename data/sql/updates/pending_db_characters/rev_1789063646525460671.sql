-- Direct sightings retain familiarity without accumulating duplicate episodes.
ALTER TABLE `alles_memory`
  ADD COLUMN `encounters` INT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN `last_seen_game_time_ms` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN `last_seen_place` VARCHAR(100) NOT NULL DEFAULT '';
