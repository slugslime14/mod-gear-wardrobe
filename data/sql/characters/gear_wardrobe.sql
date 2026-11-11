-- Items held in a character's wardrobe. The item row stays in item_instance (owner_guid unchanged); only its
-- character_inventory row is removed while it is stored, the same way mail holds items.
CREATE TABLE IF NOT EXISTS `gear_wardrobe_item` (
  `item_guid` INT UNSIGNED NOT NULL,
  `owner_guid` INT UNSIGNED NOT NULL,
  `deposited` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item_guid`),
  KEY `idx_owner` (`owner_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `gear_wardrobe_set` (
  `owner_guid` INT UNSIGNED NOT NULL,
  `set_id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(32) NOT NULL,
  `icon` VARCHAR(100) NOT NULL DEFAULT '',
  PRIMARY KEY (`owner_guid`, `set_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- One row per slot the set controls; a slot without a row is ignored when the set is equipped.
-- mode 0 = wear item_guid, mode 1 = leave the slot empty.
CREATE TABLE IF NOT EXISTS `gear_wardrobe_slot` (
  `owner_guid` INT UNSIGNED NOT NULL,
  `set_id` INT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL,
  `mode` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `item_guid` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`owner_guid`, `set_id`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Shared wardrobe (GearWardrobe.SharedPool): items taken out of the wardrobe, and who has them, so the other
-- characters still see them (as unavailable). A row goes when the item is stored again or is no longer found on
-- that character. `link` is the item string the addon shows while the holder is offline.
CREATE TABLE IF NOT EXISTS `gear_wardrobe_out` (
  `item_guid` INT UNSIGNED NOT NULL,
  `holder_guid` INT UNSIGNED NOT NULL,
  `link` VARCHAR(160) NOT NULL DEFAULT '',
  `taken` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`item_guid`),
  KEY `idx_holder` (`holder_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
