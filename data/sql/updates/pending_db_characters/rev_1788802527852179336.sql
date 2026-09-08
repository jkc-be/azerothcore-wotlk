-- Per-owner semantic intentions and private knowledge, committed with the existing alles_actor revision.
-- No existing memories or perceptions are changed. The codec validates version, owner, bounds and references.
CREATE TABLE IF NOT EXISTS `alles_planning` (
  `owner_kind` TINYINT UNSIGNED NOT NULL,
  `owner_id` BIGINT UNSIGNED NOT NULL,
  `payload` MEDIUMTEXT NOT NULL,
  PRIMARY KEY (`owner_kind`, `owner_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
