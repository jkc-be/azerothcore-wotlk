-- mod-alles current owner snapshots; no immutable event history.
-- owner_kind: 0 = player GUID, 1 = persistent creature spawn ID.
-- Install before starting any binary compiled with MOD_ALLES, even if Alles.Enable = 0.

CREATE TABLE IF NOT EXISTS `alles_actor` (
  `owner_kind` TINYINT UNSIGNED NOT NULL,
  `owner_id` BIGINT UNSIGNED NOT NULL,
  `committed_revision` BIGINT UNSIGNED NOT NULL,
  `next_perception_id` BIGINT UNSIGNED NOT NULL,
  `next_memory_id` BIGINT UNSIGNED NOT NULL,
  `decay_game_time_ms` BIGINT UNSIGNED NOT NULL,
  `next_consolidation_game_time_ms` BIGINT UNSIGNED NOT NULL,
  PRIMARY KEY (`owner_kind`, `owner_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `alles_perception` (
  `owner_kind` TINYINT UNSIGNED NOT NULL,
  `owner_id` BIGINT UNSIGNED NOT NULL,
  `perception_id` BIGINT UNSIGNED NOT NULL,
  `kind` TINYINT UNSIGNED NOT NULL,
  `subject_kind` TINYINT UNSIGNED DEFAULT NULL,
  `subject_id` BIGINT UNSIGNED DEFAULT NULL,
  `subject_name` VARCHAR(100) NOT NULL DEFAULT '',
  `source_kind` TINYINT UNSIGNED DEFAULT NULL,
  `source_id` BIGINT UNSIGNED DEFAULT NULL,
  `source_name` VARCHAR(100) NOT NULL DEFAULT '',
  `comprehended` TINYINT UNSIGNED NOT NULL,
  `language` INT UNSIGNED NOT NULL,
  `gated_text` VARCHAR(512) NOT NULL DEFAULT '',
  `place` VARCHAR(100) NOT NULL DEFAULT '',
  `game_time_ms` BIGINT UNSIGNED NOT NULL,
  `self_context` VARCHAR(2048) NOT NULL DEFAULT '',
  PRIMARY KEY (`owner_kind`, `owner_id`, `perception_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `alles_memory` (
  `owner_kind` TINYINT UNSIGNED NOT NULL,
  `owner_id` BIGINT UNSIGNED NOT NULL,
  `memory_id` BIGINT UNSIGNED NOT NULL,
  `content_revision` BIGINT UNSIGNED NOT NULL,
  `kind` TINYINT UNSIGNED NOT NULL,
  `subject_kind` TINYINT UNSIGNED DEFAULT NULL,
  `subject_id` BIGINT UNSIGNED DEFAULT NULL,
  `subject_name` VARCHAR(100) NOT NULL DEFAULT '',
  `source_kind` TINYINT UNSIGNED DEFAULT NULL,
  `source_id` BIGINT UNSIGNED DEFAULT NULL,
  `source_name` VARCHAR(100) NOT NULL DEFAULT '',
  `claim` VARCHAR(512) NOT NULL,
  `attribution` VARCHAR(100) NOT NULL DEFAULT '',
  `reported_depth` INT UNSIGNED DEFAULT NULL,
  `confidence` DOUBLE NOT NULL,
  `salience` DOUBLE NOT NULL,
  `formed_game_time_ms` BIGINT UNSIGNED NOT NULL,
  `recalled_game_time_ms` BIGINT UNSIGNED NOT NULL,
  `decay_game_time_ms` BIGINT UNSIGNED NOT NULL,
  `formation_mode` TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`owner_kind`, `owner_id`, `memory_id`),
  KEY `idx_alles_memory_owner_salience` (`owner_kind`, `owner_id`, `salience`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
