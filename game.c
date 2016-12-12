/*
* game.c
*/

#define UNUSED	-1

#define LAYER_UNSET		0
#define LAYER_GROUND	1
#define LAYER_MID		2
#define LAYER_AIR		3
#define LAYER_TOP		4

#define MONSTER_TYPE_COUNT	100
#define MAX_DUNGEON_LEVEL	20

typedef enum {
	COMP_POSITION = 0,
	COMP_VISIBILITY,
	COMP_PHYSICAL,
	COMP_HEALTH,
	COMP_MOVEMENT,
	COMP_COMBAT,

	/* Define other components above here */
	COMPONENT_COUNT
} GameComponentType;


/* Entity */
typedef struct {
	i32 id;
	void *components[COMPONENT_COUNT];
} GameObject;


/* Components */
typedef struct {
	i32 objectId;
	u8 x, y;	
	u8 layer;				// 1 is bottom layer
} Position;

typedef struct {
	i32 objectId;
	asciiChar glyph;
	u32 fgColor;
	u32 bgColor;
	bool hasBeenSeen;
	bool visibleOutsideFOV;
} Visibility;

typedef struct {
	i32 objectId;
	bool blocksMovement;
	bool blocksSight;
} Physical;

typedef struct {
	i32 objectId;
	i32 speed;				// How many spaces the object can move when it moves.
	i32 frequency;			// How often the object moves. 1=every tick, 2=every other tick, etc.
	i32 ticksUntilNextMove;	// Countdown to next move. Moves when = 0.
	Point destination;
	bool hasDestination;
	bool chasingPlayer;
	i32 turnsSincePlayerSeen;
} Movement;

typedef struct {
	i32 objectId;
	i32 currentHP;
	i32 maxHP;
	i32 recoveryRate;		// HP recovered per tick.
	i32 ticksUntilRemoval;	// Countdown to removal from world state
} Health;

typedef struct {
	i32 objectId;
	i32 attack;				// attack = damage inflicted per hit
	i32 attackModifier;		// based on weapons/items
	i32 defense;			// defense = damage absorbed before HP is affected
	i32 defenseModifier;	// based on armor/items
	i32 dodgeModifier;		// % that attack was dodged
} Combat;


/* Level Support */

typedef struct {
	i32 level;
	bool (*mapWalls)[MAP_HEIGHT];
} DungeonLevel;


/* World State */
#define MAX_GO 	10000
global_variable GameObject *player = NULL;
global_variable GameObject gameObjects[MAX_GO];
global_variable List *positionComps;
global_variable List *visibilityComps;
global_variable List *physicalComps;
global_variable List *movementComps;
global_variable List *healthComps;
global_variable List *combatComps;

global_variable DungeonLevel *currentLevel;
global_variable u32 fovMap[MAP_WIDTH][MAP_HEIGHT];
global_variable i32 (*targetMap)[MAP_HEIGHT] = NULL;
global_variable List *goPositions[MAP_WIDTH][MAP_HEIGHT];
global_variable Config *monsterConfig = NULL;
global_variable i32 monsterProbability[MONSTER_TYPE_COUNT][MAX_DUNGEON_LEVEL];		// TODO: dynamically size this based on actual count of monsters in config file
global_variable Config *levelConfig = NULL;
global_variable i32 maxMonsters[MAX_DUNGEON_LEVEL];


/* World State Management */
void world_state_init() {
	for (u32 i = 0; i < MAX_GO; i++) {
		gameObjects[i].id = UNUSED;
	}
	positionComps = list_new(free);
	visibilityComps = list_new(free);
	physicalComps = list_new(free);
	movementComps = list_new(free);
	healthComps = list_new(free);
	combatComps = list_new(free);

	// Parse necessary config files into memory
	monsterConfig = config_file_parse("monsters.cfg");
	levelConfig = config_file_parse("levels.cfg");

	// Generate our monster appearance probability data
	ListElement *e = list_head(monsterConfig->entities);
	while (e != NULL) {
		ConfigEntity *entity = (ConfigEntity *)e->data;
		char *appearance_prob = config_entity_value(entity, "appearance_prob");
		char *copy = (char *)malloc(strlen(appearance_prob) + 1);
		strcpy(copy, appearance_prob);

		char *monsterId = config_entity_value(entity, "monster_id");
		i32 monId = atoi(monsterId);

		char *lvl = strtok(copy, ",");
		if (lvl != NULL) {
			i32 lastLvl = 0;
			bool probData = true;
			while (probData) {
				char *prob = strtok(NULL, ",");
				i32 lvlNum = atoi(lvl);
				i32 probNum = atoi(prob);	

				// Fill in the probabilities from our last filled level to the current level
				for (i32 i = lastLvl; i < lvlNum; i++) {
					monsterProbability[monId-1][i] = probNum;							
				}				

				lastLvl = lvlNum;
				lvl = strtok(NULL, ",");
				if (lvl == NULL) {
					probData = false;
				}
			}
		}

		free(copy);
		e = list_next(e);
	}

	// Generate our level config data
	e = list_head(levelConfig->entities);
	if (e != NULL) {
		ConfigEntity *levelEntity = (ConfigEntity *)e->data;
		char *lvlMonsters = config_entity_value(levelEntity, "max_monsters");
		char *copy = (char *)malloc(strlen(lvlMonsters) + 1);
		strcpy(copy, lvlMonsters);

		char *lvl = strtok(copy, ",");
		if (lvl != NULL) {
			i32 lastLvl = 0;
			bool lvlMonData = true;
			while (lvlMonData) {
				char *monCount = strtok(NULL, ",");
				i32 lvlNum = atoi(lvl);
				i32 maxMon = atoi(monCount);

				// Fill in the probabilities from our last filled level to the current level
				for (i32 i = lastLvl; i < lvlNum; i++) {
					maxMonsters[i] = maxMon;
				}				

				lastLvl = lvlNum;
				lvl = strtok(NULL, ",");
				if (lvl == NULL) {
					lvlMonData = false;
				}
			}
		}

		free(copy);
	}

	// TODO: Other one-time data generation using config data?

}


/* Game Object Management */
GameObject *game_object_create() {
	// Find the next available object space
	GameObject *go = NULL;
	for (i32 i = 0; i < MAX_GO; i++) {
		if (gameObjects[i].id == UNUSED) {
			go = &gameObjects[i];
			go->id = i;
			break;
		}
	}

	assert(go != NULL);		// Have we run out of game objects?

	for (i32 i = 0; i < COMPONENT_COUNT; i++) {
		go->components[i] = NULL;
	}

	return go;
}

void game_object_update_component(GameObject *obj, 
							  GameComponentType comp,
							  void *compData) {
	assert(obj->id != UNUSED);

	switch (comp) {
		case COMP_POSITION: {
			if (compData != NULL) {
				Position *pos = obj->components[COMP_POSITION];
				if (pos == NULL) {
					pos = (Position *)malloc(sizeof(Position));
				} else {
					// Remove game obj from the position helper DS
					List *ls = goPositions[pos->x][pos->y];
					list_remove_element_with_data(ls, obj);
				}
				Position *posData = (Position *)compData;
				pos->objectId = obj->id;
				pos->x = posData->x;
				pos->y = posData->y;
				pos->layer = posData->layer;

				list_insert_after(positionComps, NULL, pos);
				obj->components[comp] = pos;

				// Update our helper DS 
				List *gos = goPositions[posData->x][posData->y];
				if (gos == NULL) {
					gos = list_new(free);
					goPositions[posData->x][posData->y] = gos;
				}
				list_insert_after(gos, NULL, obj);

			} else {
				// Clear component 
				Position *pos = obj->components[COMP_POSITION];
				if (pos != NULL) {
					list_remove_element_with_data(positionComps, pos);				
				}
				obj->components[comp] = NULL;

				// Remove game obj from the position helper DS
				List *ls = goPositions[pos->x][pos->y];
				list_remove_element_with_data(ls, obj);
			}

			break;
		}

		case COMP_VISIBILITY: {
			if (compData != NULL) {
				Visibility *vis = obj->components[COMP_VISIBILITY];
				if (vis == NULL)  {
					vis = (Visibility *)malloc(sizeof(Visibility));
				}
				Visibility *visData = (Visibility *)compData;
				vis->objectId = obj->id;
				vis->glyph = visData->glyph;
				vis->fgColor = visData->fgColor;
				vis->bgColor = visData->bgColor;
				vis->hasBeenSeen = visData->hasBeenSeen;
				vis->visibleOutsideFOV = visData->visibleOutsideFOV;

				list_insert_after(visibilityComps, NULL, vis);
				obj->components[comp] = vis;

			} else {
				// Clear component 
				Visibility *vis = obj->components[COMP_VISIBILITY];
				if (vis != NULL) {
					list_remove_element_with_data(visibilityComps, vis);				
				}
				obj->components[comp] = NULL;
			}

			break;
		}

		case COMP_PHYSICAL: {
			if (compData != NULL) {
				Physical *phys = obj->components[COMP_PHYSICAL];
				if (phys == NULL) {
					phys = (Physical *)malloc(sizeof(Physical));
				}
				Physical *physData = (Physical *)compData;
				phys->objectId = obj->id;
				phys->blocksSight = physData->blocksSight;
				phys->blocksMovement = physData->blocksMovement;

				list_insert_after(physicalComps, NULL, phys);
				obj->components[comp] = phys;

			} else {
				// Clear component 
				Physical *phys = obj->components[COMP_PHYSICAL];
				if (phys != NULL) {
					list_remove_element_with_data(physicalComps, phys);				
				}
				obj->components[comp] = NULL;
			}

			break;
		}

		case COMP_MOVEMENT: {
			if (compData != NULL) {
				Movement *mv = obj->components[COMP_MOVEMENT];
				if (mv == NULL) {
					mv = (Movement *)malloc(sizeof(Movement));
				}
				Movement *mvData = (Movement *)compData;
				mv->objectId = obj->id;
				mv->speed = mvData->speed;
				mv->frequency = mvData->frequency;
				mv->ticksUntilNextMove = mvData->ticksUntilNextMove;
				mv->chasingPlayer = mvData->chasingPlayer;
				mv->turnsSincePlayerSeen = mvData->turnsSincePlayerSeen;

				list_insert_after(movementComps, NULL, mv);
				obj->components[comp] = mv;

			} else {
				// Clear component 
				Movement *mv = obj->components[COMP_MOVEMENT];
				if (mv != NULL) {
					list_remove_element_with_data(movementComps, mv);				
				}
				obj->components[comp] = NULL;				
			}

			break;
		}

		case COMP_HEALTH: {
			if (compData != NULL) {
				Health *hlth = obj->components[COMP_HEALTH];
				if (hlth == NULL) {
					hlth = (Health *)malloc(sizeof(Health));
				}
				Health *hlthData = (Health *)compData;
				hlth->objectId = obj->id;
				hlth->currentHP = hlthData->currentHP;
				hlth->maxHP = hlthData->maxHP;
				hlth->recoveryRate = hlthData->recoveryRate;

				list_insert_after(healthComps, NULL, hlth);
				obj->components[comp] = hlth;

			} else {
				// Clear component 
				Health *h = obj->components[COMP_HEALTH];
				if (h != NULL) {
					list_remove_element_with_data(healthComps, h);				
				}
				obj->components[comp] = NULL;				
			}

			break;
		}

		case COMP_COMBAT: {
			if (compData != NULL) {
				Combat *com = obj->components[COMP_COMBAT];
				if (com == NULL) {
					com = (Combat *)malloc(sizeof(Combat));
				}
				Combat *combatData = (Combat *)compData;
				com->objectId = obj->id;
				com->attack = combatData->attack;
				com->defense = combatData->defense;

				list_insert_after(combatComps, NULL, com);
				obj->components[comp] = com;
				
			} else {
				// Clear component 
				Combat *c = obj->components[COMP_COMBAT];
				if (c != NULL) {
					list_remove_element_with_data(combatComps, c);				
				}
				obj->components[comp] = NULL;				
			}

			break;
		}

		default:
			assert(1 == 0);
	}

}

void game_object_destroy(GameObject *obj) {
	ListElement *elementToRemove = list_search(positionComps, obj->components[COMP_POSITION]);
	if (elementToRemove != NULL ) { list_remove(positionComps, elementToRemove); }

	elementToRemove = list_search(visibilityComps, obj->components[COMP_VISIBILITY]);
	if (elementToRemove != NULL ) { list_remove(visibilityComps, elementToRemove); }

	elementToRemove = list_search(physicalComps, obj->components[COMP_PHYSICAL]);
	if (elementToRemove != NULL ) { list_remove(physicalComps, elementToRemove); }

	elementToRemove = list_search(movementComps, obj->components[COMP_MOVEMENT]);
	if (elementToRemove != NULL ) { list_remove(movementComps, elementToRemove); }

	elementToRemove = list_search(healthComps, obj->components[COMP_HEALTH]);
	if (elementToRemove != NULL ) { list_remove(healthComps, elementToRemove); }

	elementToRemove = list_search(combatComps, obj->components[COMP_COMBAT]);
	if (elementToRemove != NULL ) { list_remove(combatComps, elementToRemove); }

	// TODO: Clean up other components used by this object

	obj->id = UNUSED;
	for (i32 i = 0; i < COMPONENT_COUNT; i++) {
		obj->components[i] = NULL;
	}
}


void *game_object_get_component(GameObject *obj, 
								GameComponentType comp) {
	return obj->components[comp];
}

List *game_objects_at_position(u32 x, u32 y) {
	return goPositions[x][y];
}


/* Game objects */

void floor_add(u8 x, u8 y) {
	GameObject *floor = game_object_create();
	Position floorPos = {.objectId = floor->id, .x = x, .y = y, .layer = LAYER_GROUND};
	game_object_update_component(floor, COMP_POSITION, &floorPos);
	Visibility floorVis = {.objectId = floor->id, .glyph = '.', .fgColor = 0x3e3c3cFF, .bgColor = 0x00000000, .visibleOutsideFOV = true};
	game_object_update_component(floor, COMP_VISIBILITY, &floorVis);
	Physical floorPhys = {.objectId = floor->id, .blocksMovement = false, .blocksSight = false};
	game_object_update_component(floor, COMP_PHYSICAL, &floorPhys);
}

void npc_add(u8 x, u8 y, u8 layer, asciiChar glyph, u32 fgColor, u32 speed, u32 frequency, i32 maxHP, i32 hpRecRate, i32 attack, i32 defense) {
	GameObject *npc = game_object_create();
	Position pos = {.objectId = npc->id, .x = x, .y = y, .layer = layer};
	game_object_update_component(npc, COMP_POSITION, &pos);
	Visibility vis = {.objectId = npc->id, .glyph = glyph, .fgColor = fgColor, .bgColor = 0x00000000, .visibleOutsideFOV = false};
	game_object_update_component(npc, COMP_VISIBILITY, &vis);
	Physical phys = {.objectId = npc->id, .blocksMovement = true, .blocksSight = false};
	game_object_update_component(npc, COMP_PHYSICAL, &phys);
	Movement mv = {.objectId = npc->id, .speed = speed, .frequency = frequency, .ticksUntilNextMove = frequency, 
		.chasingPlayer = false, .turnsSincePlayerSeen = 0};
	game_object_update_component(npc, COMP_MOVEMENT, &mv);
	Health hlth = {.objectId = npc->id, .currentHP = maxHP, .maxHP = maxHP, .recoveryRate = hpRecRate};
	game_object_update_component(npc, COMP_HEALTH, &hlth);
	Combat com = {.objectId = npc->id, .attack = attack, .defense = defense};
	game_object_update_component(npc, COMP_COMBAT, &com);
}

void wall_add(u8 x, u8 y) {
	GameObject *wall = game_object_create();
	Position wallPos = {.objectId = wall->id, .x = x, .y = y, .layer = LAYER_GROUND};
	game_object_update_component(wall, COMP_POSITION, &wallPos);
	Visibility wallVis = {wall->id, '#', 0x675644FF, 0x00000000, .visibleOutsideFOV = true};
	game_object_update_component(wall, COMP_VISIBILITY, &wallVis);
	Physical wallPhys = {wall->id, true, true};
	game_object_update_component(wall, COMP_PHYSICAL, &wallPhys);
}


/* Level Management */

Point level_get_open_point(bool (*mapCells)[MAP_HEIGHT]) {
	// Return a random position within the level that is open
	for (;;) {
		u32 x = rand() % MAP_WIDTH;
		u32 y = rand() % MAP_HEIGHT;
		if (mapCells[x][y] == false) {
			return (Point) {x, y};
		}
	}
}

// TODO: Move this somewhere appropriate
i32 monster_for_level(i32 level) {
	u32 r = rand() % 100;
	u32 accum = 0;
	for (int i = 0; i < MONSTER_TYPE_COUNT; i++) {
		accum += monsterProbability[i][level-1];
		if (accum >= r) {
			return i + 1;
		}
	}
	return 1;
}

ConfigEntity * get_monster_config(Config *config, i32 id) {
	ListElement *e = list_head(config->entities);
	while (e != NULL) {
		ConfigEntity *entity = (ConfigEntity *)e->data;
		i32 monsterId = atoi(config_entity_value(entity, "monster_id"));
		if (monsterId == id) {
			return entity;
		}

		e = list_next(e);
	}

	return NULL;
}


DungeonLevel * level_init(i32 levelToGenerate, GameObject *player) {
	// Clear the previous level data from the world state
	// Note: We start at index 1 because the player is at index 0 and we want to keep them!
	for (u32 i = 1; i < MAX_GO; i++) {
		if ((gameObjects[i].id != player->id) && 
			(gameObjects[i].id != UNUSED)) {
			game_object_destroy(&gameObjects[i]);
		}
	}

	// Generate a level map into the world state
	bool (*mapCells)[MAP_HEIGHT] = malloc(MAP_WIDTH * MAP_HEIGHT);

	map_generate(mapCells);

	for (u32 x = 0; x < MAP_WIDTH; x++) {
		for (u32 y = 0; y < MAP_HEIGHT; y++) {
			if (mapCells[x][y]) {
				wall_add(x, y);
			} else {
				floor_add(x, y);
			}
		}
	}

	// Create DungeonLevel Object and store relevant info
	DungeonLevel *level = malloc(sizeof(DungeonLevel));
	level->level = levelToGenerate;
	level->mapWalls = mapCells;

	// Grab the number of monsters to generate for this level from level config
	i32 monstersToAdd = maxMonsters[levelToGenerate-1];
	for (i32 i = 0; i < monstersToAdd; i++) {
		// Consult our monster appearance data to determine what monster to generate.
		i32 monsterId = monster_for_level(levelToGenerate);
		ConfigEntity *monsterEntity = get_monster_config(monsterConfig, monsterId);

		if (monsterConfig != NULL) {
			// Add the monster		
			Point pt = level_get_open_point(mapCells);
			char *glyph = config_entity_value(monsterEntity, "vis_glyph");
			asciiChar g = *glyph;
			char *color = config_entity_value(monsterEntity, "vis_color");
			u32 c = xtoi(color);
			char *speed = config_entity_value(monsterEntity, "mv_speed");
			u32 s = atoi(speed);
			char *freq = config_entity_value(monsterEntity, "mv_frequency");
			u32 f = atoi(freq);
			char *maxHP = config_entity_value(monsterEntity, "h_maxHP");
			i32 hp = atoi(maxHP);
			char *recRate = config_entity_value(monsterEntity, "h_recRate");
			i32 rr = atoi(recRate);
			i32 att = atoi(config_entity_value(monsterEntity, "com_attack"));
			i32 def = atoi(config_entity_value(monsterEntity, "com_defense"));

			npc_add(pt.x, pt.y, LAYER_TOP, g, c, s, f, hp, rr, att, def);
		}
	}
	
	// Place our player in a random position in the level
	Point pt = level_get_open_point(mapCells);
	Position pos = {.objectId = player->id, .x = pt.x, .y = pt.y, .layer = LAYER_TOP};
	game_object_update_component(player, COMP_POSITION, &pos);

	return level;
}



// TODO: Need a level cleanup function 


/* Movement System */

bool can_move(Position pos) {
	bool moveAllowed = true;

	if ((pos.x >= 0) && (pos.x < NUM_COLS) && (pos.y >= 0) && (pos.y < NUM_ROWS)) {
		ListElement *e = list_head(positionComps);
		while (e != NULL) {
			Position *p = (Position *)list_data(e);
			if (p->x == pos.x && p->y == pos.y) {
				Physical *phys = (Physical *)game_object_get_component(&gameObjects[p->objectId], COMP_PHYSICAL);
				if (phys->blocksMovement == true) {
					moveAllowed = false;
					break;
				}
			}		
			e = list_next(e);
		}

	} else {
		moveAllowed = false;
	}

	return moveAllowed;
}


typedef struct {
	Point target;
	i32 weight;
} TargetPoint;

bool is_wall(i32 x, i32 y) {
	return currentLevel->mapWalls[x][y];
}

// TODO: Allow for a list of target points to be provided, with differing starting weights/priorities?
void generate_target_map(i32 targetX, i32 targetY) { // List *targetPoints) {
	// Clean up any previously generated target map
	if (targetMap != NULL) {
		free(targetMap);
	}

	i32 (* dmap)[MAP_HEIGHT] = malloc(MAP_WIDTH * MAP_HEIGHT * sizeof(i32));
	i32 UNSET = 9999;

	for (i32 x = 0; x < MAP_WIDTH; x++) {
		for (i32 y = 0; y < MAP_HEIGHT; y++) {
			dmap[x][y] = UNSET;
		}
	}

	// Set our target point(s)
	dmap[targetX][targetY] = 0;

	// Calculate our target map
	bool changesMade = true;
	while (changesMade) {
		changesMade = false;

		for (i32 x = 0; x < MAP_WIDTH; x++) {
			for (i32 y = 0; y < MAP_HEIGHT; y++) {
				i32 currCellValue = dmap[x][y];
				if (currCellValue != UNSET) {
					// Check cells around this one and update them if warranted
					if ((!is_wall(x+1, y)) && (dmap[x+1][y] > currCellValue + 1)) { 
						dmap[x+1][y] = currCellValue + 1;
						changesMade = true;
					}
					if ((!is_wall(x-1, y)) && (dmap[x-1][y] > currCellValue + 1)) { 
						dmap[x-1][y] = currCellValue + 1;
						changesMade = true;
					}
					if ((!is_wall(x, y-1)) && (dmap[x][y-1] > currCellValue + 1)) { 
						dmap[x][y-1] = currCellValue + 1;
						changesMade = true;
					}
					if ((!is_wall(x, y+1)) && (dmap[x][y+1] > currCellValue + 1)) { 
						dmap[x][y+1] = currCellValue + 1;
						changesMade = true;
					}
				}
			}
		}
	}

	targetMap = dmap;
}

void movement_update() {

	ListElement *e = list_head(movementComps);
	while (e != NULL) {
		Movement *mv = (Movement *)list_data(e);

		// Determine if the object is going to move this tick
		mv->ticksUntilNextMove -= 1;
		if (mv->ticksUntilNextMove <= 0) {
			// The object is moving, so determine new position based on destination and speed
			Position *p = (Position *)game_object_get_component(&gameObjects[mv->objectId], COMP_POSITION);
			Position newPos = {.objectId = p->objectId, .x = p->x, .y = p->y, .layer = p->layer};

			// A monster should only move toward the player if they have seen the player
			// Should give chase if player is currently in view or has been in view in the last 5 turns

			// If the player can see the monster, the monster can see the player
			bool giveChase = false;
			if (fovMap[p->x][p->y] > 0) {
				// Player is visible
				giveChase = true;
				mv->chasingPlayer = true;
				mv->turnsSincePlayerSeen = 0;
			} else {
				// Player is not visible - see if monster should still chase
				giveChase = mv->chasingPlayer;
				mv->turnsSincePlayerSeen += 1;
				if (mv->turnsSincePlayerSeen > 5) {
					mv->chasingPlayer = false;
				}
			}

			// Determine new position based on our target map
			if (giveChase) {
				// Evaluate all cardinal direction cells and pick randomly between optimal moves 
				Position moves[4];
				i32 moveCount = 0;
				i32 currTargetValue = targetMap[p->x][p->y];
				if (targetMap[p->x - 1][p->y] < currTargetValue) {
					Position np = newPos;
					np.x -= 1;	
					moves[moveCount] = np;					
					moveCount += 1;
				}
				if (targetMap[p->x][p->y - 1] < currTargetValue) { 
					Position np = newPos;
					np.y -= 1;						
					moves[moveCount] = np;					
					moveCount += 1;
				}
				if (targetMap[p->x + 1][p->y] < currTargetValue) { 
					Position np = newPos;
					np.x += 1;						
					moves[moveCount] = np;					
					moveCount += 1;
				}
				if (targetMap[p->x][p->y + 1] < currTargetValue) { 
					Position np = newPos;
					np.y += 1;						
					moves[moveCount] = np;					
					moveCount += 1;
				}

				u32 moveIdx = rand() % moveCount;
				newPos = moves[moveIdx];

			} else {
				// Move randomly?
				u32 dir = rand() % 4;
				switch (dir) {
					case 0:
						newPos.x -= 1;
						break;
					case 1:
						newPos.y -= 1;
						break;
					case 2:
						newPos.x += 1;
						break;
					default: 
						newPos.y += 1;
				}
			}

			// Test to see if the new position can be moved to
			if (can_move(newPos)) {
				game_object_update_component(&gameObjects[mv->objectId], COMP_POSITION, &newPos);
				mv->ticksUntilNextMove = mv->frequency;				
			} else {
				mv->ticksUntilNextMove += 1;
			}
		}

		e = list_next(e);
	}


}


/* Health Routines */

void health_check_death(GameObject *go) {
	Health *h = (Health *)game_object_get_component(go, COMP_HEALTH);
	if (h->currentHP <= 0) {
		// Death!
		if (go == player) {
			// TODO: Enter endgame flow

		} else {
			Visibility *vis = (Visibility *)game_object_get_component(go, COMP_VISIBILITY);
			vis->glyph = '%';
			vis->fgColor = 0x990000FF;

			Physical *phys = (Physical *)game_object_get_component(go, COMP_PHYSICAL);
			phys->blocksMovement = false;
			phys->blocksSight = false;

			// Remove the movement component - no more moving!
			game_object_update_component(go, COMP_MOVEMENT, NULL);

			h->ticksUntilRemoval = 5;
		}
	}
}

void health_recover() {
	// Loop through all our health components and apply recovery HP (only if object is not already dead)
	ListElement *e = list_head(healthComps);
	while (e != NULL) {
		Health *h = (Health *)list_data(e);
		if (h->currentHP > 0) {
			h->currentHP += h->recoveryRate;
			if (h->currentHP > h->maxHP) { 
				h->currentHP = h->maxHP;
			}			
		}
		e = list_next(e);
	}
}

void health_removal_update() {
	// Loop through all our health components and remove any objects that have been dead for awhile. 
	// Decrement counters for newly-dead objects
	ListElement *e = list_head(healthComps);
	while (e != NULL) {
		Health *h = (Health *)list_data(e);
		if (h->currentHP <= 0) {
			if (h->ticksUntilRemoval <= 0) {
				// Remove object and all related components from world state
				GameObject goToDestroy = gameObjects[h->objectId];
				game_object_destroy(&goToDestroy);

			} else {
				h->ticksUntilRemoval -= 1;
			}
		}
		e = list_next(e);
	}
}


/* Combat Routines */

void combat_deal_damage(GameObject *attacker, GameObject *defender) {
	Combat *att = (Combat *)game_object_get_component(attacker, COMP_COMBAT);
	Combat *def = (Combat *)game_object_get_component(defender, COMP_COMBAT);
	Health *defHealth = (Health *)game_object_get_component(defender, COMP_HEALTH);

	i32 totAtt = att->attack + att->attackModifier;
	i32 totDef = def->defense + def->defenseModifier;

	if (totDef >= totAtt) {
		// TODO: Show a message about no damage dealt

	} else {
		defHealth->currentHP -= (totAtt - totDef);
		health_check_death(defender);
	}
}

void combat_attack(GameObject *attacker, GameObject *defender) {
	Combat *att = (Combat *)game_object_get_component(attacker, COMP_COMBAT);
	Combat *def = (Combat *)game_object_get_component(defender, COMP_COMBAT);

	u32 hitRoll = rand() % 100;
	i32 hitWindow = 99 - def->dodgeModifier;
	if (hitRoll < hitWindow) {
		// We have a hit
		combat_deal_damage(attacker, defender);

	} else {
		// Miss
		// TODO: Show a message stating that the attack was a miss.
	}

}