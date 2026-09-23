#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#define MAX_PLY 64
#define MATE 32000
#define INF 32001
#define S8 signed __int8
#define U8 unsigned __int8
#define S16 signed __int16
#define U16 unsigned __int16
#define S32 signed __int32
#define S64 signed __int64
#define U64 unsigned __int64
#define FALSE 0
#define TRUE 1
#define NAME "Crab"
#define VERSION "2026-08-02"
#define START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


enum { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H };
enum { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8 };
enum Color { WHITE, BLACK, COLOR_NB };
enum PieceType { PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, PT_NB };
enum Bound { UPPER, LOWER, EXACT };

typedef struct {
	U8 flipped;
	U8 move50;
	U64 castling[4];
	U64 color[COLOR_NB];
	U64 pieces[PT_NB];
	U64 ep;
}Position;

typedef struct {
	U8 from;
	U8 to;
	U8 promo;
}Move;

typedef struct {
	Move move;
	Move killer1;
	Move killer2;
} Stack;

typedef struct {
	U64 hash;
	Move move;
	S16 score;
	U8 depth;
	U8 flag;
}TTEntry;

typedef struct {
	U8 post;
	U8 stop;
	U8 depthLimit;
	U64 timeStart;
	U64 timeLimit;
	U64 nodes;
	U64 nodesLimit;
}SearchInfo;

static const U64 FILE_ABB = 0x0101010101010101ULL;
static const U64 FILE_BBB = 0x0202020202020202ULL;
static const U64 FILE_CBB = 0x0404040404040404ULL;
static const U64 FILE_DBB = 0x0808080808080808ULL;
static const U64 FILE_EBB = 0x1010101010101010ULL;
static const U64 FILE_FBB = 0x2020202020202020ULL;
static const U64 FILE_GBB = 0x4040404040404040ULL;
static const U64 FILE_HBB = 0x8080808080808080ULL;

U64 bbRanks[8] = {
	0x00000000000000ffULL,
	0x000000000000ff00ULL,
	0x0000000000ff0000ULL,
	0x00000000ff000000ULL,
	0x000000ff00000000ULL,
	0x0000ff0000000000ULL,
	0x00ff000000000000ULL,
	0xff00000000000000ULL };

U64 bbFiles[8] = {
	0x0101010101010101ULL,
	0x0202020202020202ULL,
	0x0404040404040404ULL,
	0x0808080808080808ULL,
	0x1010101010101010ULL,
	0x2020202020202020ULL,
	0x4040404040404040ULL,
	0x8080808080808080ULL };

int mg_material[PT_NB] = { 82, 337, 365, 477, 1025, 0 };
int eg_material[PT_NB] = { 94, 281, 297, 512,  936, 0 };
int mx_material[PT_NB] = { 94, 337, 365, 512, 1025, 0 };
int insufVal[PT_NB] = { 5,2,3,5,5,0 };
int phaseVal[PT_NB] = { 0,1,1,2,4,0 };
int historyCount = 0;
U64 keys[848];
U64 historyHash[1024];
const U64 tt_count = 64ULL << 15;
U64 bbSquare[64];
U64 bbKnightAttack[64];
U64 bbKingAttack[64];
int hh[2][64][64];
int mg_pst[PT_NB][64];
int eg_pst[PT_NB][64];
SearchInfo info;
Stack ss[128];
TTEntry tt[64ULL << 15];

void UciCommand(Position* pos, char* line);

static inline void TTClear() { memset(tt, 0, sizeof(tt)); }
static inline void HHClear() { memset(hh, 0, sizeof(hh)); }
static inline void SSClear() { memset(ss, 0, sizeof(ss)); }
static inline U64 GetTimeMs() { return GetTickCount64(); }
static inline U64 FlipBitboard(const U64 bb) { return _byteswap_uint64(bb); }
static inline U64 LSB(const U64 bb) { return _tzcnt_u64(bb); }
static inline U64 Count(const U64 bb) { return _mm_popcnt_u64(bb); }
static inline U64 East(const U64 bb) { return (bb << 1) & ~FILE_ABB; }
static inline U64 West(const U64 bb) { return (bb >> 1) & ~FILE_HBB; }
static inline U64 North(const U64 bb) { return bb << 8; }
static inline U64 South(const U64 bb) { return bb >> 8; }
static inline U64 NW(const U64 bb) { return (bb << 7) & ~FILE_HBB; }
static inline U64 NE(const U64 bb) { return (bb << 9) & ~FILE_ABB; }
static inline U64 SW(const U64 bb) { return (bb >> 9) & ~FILE_HBB; }
static inline U64 SE(const U64 bb) { return (bb >> 7) & ~FILE_ABB; }
static inline int FileOf(int sq) { return sq % 8; }
static inline int RankOf(int sq) { return sq / 8; }
static inline int Center(int rank, int file) { return -abs(rank * 2 - 7) / 2 - abs(file * 2 - 7) / 2; }
static inline int CenterSq(int sq) { return Center(RankOf(sq), FileOf(sq)); }
static inline int Equal(const Move lhs, const Move rhs) { return !memcmp(&rhs, &lhs, sizeof(Move)); }

static void Swap(U64* a, U64* b) {
	U64 temp = *a;
	*a = *b;
	*b = temp;
}

static int PieceTypeOnSquare(Position* pos, int sq) {
	const U64 bb = bbSquare[sq];
	for (int i = PAWN; i < PT_NB; ++i)
		if (pos->pieces[i] & bb)
			return i;
	return PT_NB;
}

static U64 Ray(const U64 bb, const U64 blockers, U64(*f)(U64)) {
	U64 mask = f(bb);
	mask |= f(mask & ~blockers);
	mask |= f(mask & ~blockers);
	mask |= f(mask & ~blockers);
	mask |= f(mask & ~blockers);
	mask |= f(mask & ~blockers);
	mask |= f(mask & ~blockers);
	mask |= f(mask & ~blockers);
	return mask;
}

static U64 KnightAttackBB(const U64 bb) {
	return (((bb << 15) | (bb >> 17)) & 0x7F7F7F7F7F7F7F7FULL) | (((bb << 17) | (bb >> 15)) & 0xFEFEFEFEFEFEFEFEULL) |
		(((bb << 10) | (bb >> 6)) & 0xFCFCFCFCFCFCFCFCULL) | (((bb << 6) | (bb >> 10)) & 0x3F3F3F3F3F3F3F3FULL);
}

static U64 KnightAttack(const int sq) {
	return bbKnightAttack[sq];
}

static U64 BishopAttackBB(U64 bb, U64 blockers) {
	return Ray(bb, blockers, NW) | Ray(bb, blockers, NE) | Ray(bb, blockers, SW) | Ray(bb, blockers, SE);
}

static U64 BishopAttack(const int sq, const U64 blockers) {
	return BishopAttackBB(bbSquare[sq], blockers);
}

static U64 RookAttackBB(U64 bb, U64 blockers) {
	return Ray(bb, blockers, North) | Ray(bb, blockers, East) | Ray(bb, blockers, South) | Ray(bb, blockers, West);
}

static U64 RookAttack(const int sq, const U64 blockers) {
	return RookAttackBB(bbSquare[sq], blockers);
}

static U64 KingAttackBB(const U64 bb) {
	return (bb << 8) | (bb >> 8) | (((bb >> 1) | (bb >> 9) | (bb << 7)) & 0x7F7F7F7F7F7F7F7FULL) |
		(((bb << 1) | (bb << 9) | (bb >> 7)) & 0xFEFEFEFEFEFEFEFEULL);
}

static U64 KingAttack(const int sq) {
	return bbKingAttack[sq];
}

static void FlipPosition(Position* pos) {
	pos->color[0] = FlipBitboard(pos->color[0]);
	pos->color[1] = FlipBitboard(pos->color[1]);
	for (int i = PAWN; i < PT_NB; ++i)
		pos->pieces[i] = FlipBitboard(pos->pieces[i]);
	pos->ep = FlipBitboard(pos->ep);
	Swap(&pos->color[0], &pos->color[1]);
	Swap(&pos->castling[0], &pos->castling[2]);
	Swap(&pos->castling[1], &pos->castling[3]);
	pos->flipped = !pos->flipped;
}

static U64 GetHash(const Position* pos) {
	U64 hash = pos->flipped;
	for (S32 pt = PAWN; pt < PT_NB; ++pt) {
		U64 copy = pos->pieces[pt] & pos->color[0];
		while (copy) {
			const S32 sq = LSB(copy);
			copy &= copy - 1;
			hash ^= keys[pt * 64 + sq];
		}
		copy = pos->pieces[pt] & pos->color[1];
		while (copy) {
			const S32 sq = LSB(copy);
			copy &= copy - 1;
			hash ^= keys[(pt + 6) * 64 + sq];
		}
	}
	if (pos->ep)
		hash ^= keys[12 * 64 + LSB(pos->ep)];
	hash ^= keys[13 * 64 + pos->castling[0] + pos->castling[1] * 2 + pos->castling[2] * 4 + pos->castling[3] * 8];
	return hash;
}

static void PrintBoard(Position* pos) {
	Position npos = *pos;
	if (npos.flipped)
		FlipPosition(&npos);
	const char* s = "   +---+---+---+---+---+---+---+---+\n";
	const char* t = "     A   B   C   D   E   F   G   H\n";
	printf(t);
	for (int r = 7; r >= 0; r--) {
		printf(s);
		printf(" %d |", r + 1);
		for (int f = 0; f < 8; f++) {
			int sq = r * 8 + f;
			int piece = PieceTypeOnSquare(&npos, sq);
			if (npos.color[0] & (1ull << sq))
				printf(" %c |", "ANBRQK "[piece]);
			else
				printf(" %c |", "anbrqk "[piece]);
		}
		printf(" %d \n", r + 1);
	}
	printf(s);
	printf(t);
	char castling[5] = "KQkq";
	for (int n = 0; n < 4; n++)
		if (!npos.castling[n])
			castling[n] = '-';
	printf("side     : %16s\n", pos->flipped ? "black" : "white");
	printf("castling : %16s\n", castling);
	printf("hash     : %16llx\n", GetHash(pos));
	printf("score    : %16d\n", EvalPosition(pos));
}

static int InputAvailable(void) {
	static int init = 0, pipe;
	static HANDLE inh;
	DWORD dw;
	if (!init) {
		init = 1;
		inh = GetStdHandle(STD_INPUT_HANDLE);
		pipe = !GetConsoleMode(inh, &dw);
		if (!pipe) {
			SetConsoleMode(inh, dw & ~(ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT));
			FlushConsoleInputBuffer(inh);
		}
	}
	if (pipe) {
		if (!PeekNamedPipe(inh, NULL, 0, NULL, &dw, NULL))
			return 1;
		return dw > 0;
	}
	else {
		GetNumberOfConsoleInputEvents(inh, &dw);
		return dw > 1;
	}
}

static int CheckUp(Position* pos) {
	if ((++info.nodes & 0xffff) == 0) {
		if (info.timeLimit && GetTimeMs() - info.timeStart > info.timeLimit)
			info.stop = TRUE;
		if (info.nodesLimit && info.nodes > info.nodesLimit)
			info.stop = TRUE;
		if (InputAvailable()) {
			char line[4000];
			fgets(line, sizeof(line), stdin);
			UciCommand(pos, line);
		}
	}
	return info.stop;
}

static U64 Attacked(Position* pos, int sq, int them) {
	const U64 bb = bbSquare[sq];
	const U64 kt = pos->color[them] & pos->pieces[KNIGHT];
	const U64 BQ = pos->pieces[BISHOP] | pos->pieces[QUEEN];
	const U64 RQ = pos->pieces[ROOK] | pos->pieces[QUEEN];
	const U64 pawns = pos->color[them] & pos->pieces[PAWN];
	const U64 pawn_attacks = them ? SW(pawns) | SE(pawns) : NW(pawns) | NE(pawns);
	return (pawn_attacks & bb) | (kt & KnightAttack(sq)) |
		(BishopAttack(sq, pos->color[0] | pos->color[1]) & pos->color[them] & BQ) |
		(RookAttack(sq, pos->color[0] | pos->color[1]) & pos->color[them] & RQ) |
		(KingAttack(sq) & pos->color[them] & pos->pieces[KING]);
}

static void AddMove(Move* const moveList, int* num_moves, const int from, const int to, const int promo) {
	Move* m = &moveList[(*num_moves)++];
	m->from = from;
	m->to = to;
	m->promo = promo;
}

static void GeneratePawnMoves(Move* const moveList, int* num_moves, U64 to_mask, const int offset) {
	while (to_mask) {
		const U64 to = LSB(to_mask);
		to_mask &= to_mask - 1;
		if (to >= 56) {
			AddMove(moveList, num_moves, to + offset, to, KNIGHT);
			AddMove(moveList, num_moves, to + offset, to, BISHOP);
			AddMove(moveList, num_moves, to + offset, to, ROOK);
			AddMove(moveList, num_moves, to + offset, to, QUEEN);
		}
		else
			AddMove(moveList, num_moves, to + offset, to, PT_NB);
	}
}

static void GeneratePieceMoves(Move* const moveList, int* num_moves, const Position* pos, const int piece, const U64 to_mask, U64(*func)(int, U64)) {
	U64 copy = pos->color[0] & pos->pieces[piece];
	while (copy) {
		const int fr = (int)LSB(copy);
		copy &= copy - 1;
		U64 moves = func(fr, pos->color[0] | pos->color[1]) & to_mask;
		while (moves) {
			const int to = (int)LSB(moves);
			moves &= moves - 1;
			AddMove(moveList, num_moves, fr, to, PT_NB);
		}
	}
}

static int MoveGen(const Position* pos, Move* const moveList, int only_captures) {
	int num_moves = 0;
	const U64 all = pos->color[0] | pos->color[1];
	const U64 to_mask = only_captures ? pos->color[1] : ~pos->color[0];
	const U64 pawnsUs = pos->color[0] & pos->pieces[PAWN];
	U64 maskToPawn = North(pawnsUs) & ~all & (only_captures ? 0xFF00000000000000ULL : 0xFFFFFFFFFFFF0000ULL);
	GeneratePawnMoves(moveList, &num_moves, maskToPawn, -8);
	if (!only_captures)
		GeneratePawnMoves(moveList, &num_moves, North(North(pawnsUs & 0xFF00ULL) & ~all) & ~all, -16);
	GeneratePawnMoves(moveList, &num_moves, NW(pawnsUs) & (pos->color[1] | pos->ep), -7);
	GeneratePawnMoves(moveList, &num_moves, NE(pawnsUs) & (pos->color[1] | pos->ep), -9);
	GeneratePieceMoves(moveList, &num_moves, pos, KNIGHT, to_mask, KnightAttack);
	GeneratePieceMoves(moveList, &num_moves, pos, BISHOP, to_mask, BishopAttack);
	GeneratePieceMoves(moveList, &num_moves, pos, QUEEN, to_mask, BishopAttack);
	GeneratePieceMoves(moveList, &num_moves, pos, ROOK, to_mask, RookAttack);
	GeneratePieceMoves(moveList, &num_moves, pos, QUEEN, to_mask, RookAttack);
	GeneratePieceMoves(moveList, &num_moves, pos, KING, to_mask, KingAttack);
	if (!only_captures && pos->castling[0] && !(all & 0x60ULL) && !Attacked(pos, 4, 1) && !Attacked(pos, 5, 1))
		AddMove(moveList, &num_moves, 4, 6, PT_NB);
	if (!only_captures && pos->castling[1] && !(all & 0xEULL) && !Attacked(pos, 4, 1) && !Attacked(pos, 3, 1))
		AddMove(moveList, &num_moves, 4, 2, PT_NB);
	return num_moves;
}

static int IsRepetition(Position* pos, U64 hash) {
	int limit = max(0, historyCount - pos->move50);
	for (int n = historyCount - 4; n >= limit; n -= 2)
		if (historyHash[n] == hash)
			return TRUE;
	return FALSE;
}

static U64 Rand64() {
	static U64 next = 1;
	next = next * 12345104729 + 104723;
	return next;
}

static void InitBitboards() {
	for (int sq = 0; sq < 64; ++sq) {
		U64 bb = 1ULL << sq;
		bbSquare[sq] = bb;
		bbKnightAttack[sq] = KnightAttackBB(bb);
		bbKingAttack[sq] = KingAttackBB(bb);
	}
}

static void InitHash() {
	for (int i = 0; i < 848; ++i)
		keys[i] = Rand64();
}

static void InitPst() {
	for (int pt = PAWN; pt <= KING; pt++)
		for (int sq = 0; sq < 64; sq++) {
			int mg = mg_material[pt];
			int eg = eg_material[pt];
			int file = FileOf(sq);
			int rank = RankOf(sq);
			int center = Center(rank, file);
			switch (pt) {
			case PAWN:
				mg += file == FILE_D || file == FILE_E ? rank : 0;
				eg += rank;
				break;
			case KNIGHT:
			case BISHOP:
			case ROOK:
			case QUEEN:
				mg += center;
				eg += center;
				break;
			case KING:
				mg -= center;
				eg += center;
				break;
			}
			mg_pst[pt][sq] = mg;
			eg_pst[pt][sq] = eg;
		}
}

static void SetFen(Position* pos, char* fen) {
	memset(pos, 0, sizeof(Position));
	int sq = 56;
	while (*fen && *fen != ' ') {
		U64 bb = 1ull << sq;
		switch (*fen) {
		case '1': sq += 1; break;
		case '2': sq += 2; break;
		case '3': sq += 3; break;
		case '4': sq += 4; break;
		case '5': sq += 5; break;
		case '6': sq += 6; break;
		case '7': sq += 7; break;
		case '8': sq += 8; break;
		case 'P': pos->color[0] |= bb; pos->pieces[PAWN] |= bb; ++sq; break;
		case 'N': pos->color[0] |= bb; pos->pieces[KNIGHT] |= bb; ++sq; break;
		case 'B': pos->color[0] |= bb; pos->pieces[BISHOP] |= bb; ++sq; break;
		case 'R': pos->color[0] |= bb; pos->pieces[ROOK] |= bb; ++sq; break;
		case 'Q': pos->color[0] |= bb; pos->pieces[QUEEN] |= bb; ++sq; break;
		case 'K': pos->color[0] |= bb; pos->pieces[KING] |= bb; ++sq; break;
		case 'p': pos->color[1] |= bb; pos->pieces[PAWN] |= bb; ++sq; break;
		case 'n': pos->color[1] |= bb; pos->pieces[KNIGHT] |= bb; ++sq; break;
		case 'b': pos->color[1] |= bb; pos->pieces[BISHOP] |= bb; ++sq; break;
		case 'r': pos->color[1] |= bb; pos->pieces[ROOK] |= bb; ++sq; break;
		case 'q': pos->color[1] |= bb; pos->pieces[QUEEN] |= bb; ++sq; break;
		case 'k': pos->color[1] |= bb; pos->pieces[KING] |= bb; ++sq; break;
		case '/': sq -= 16; break;
		}
		fen++;
	}
	fen++;
	int flipped = *fen == 'w' ? WHITE : BLACK;
	while (*fen && *fen != ' ')
		fen++;
	fen++;
	while (*fen && *fen != ' ') {
		switch (*fen) {
		case 'K': pos->castling[0] = 1; break;
		case 'Q': pos->castling[1] = 1; break;
		case 'k': pos->castling[2] = 1; break;
		case 'q': pos->castling[3] = 1; break;
		case '-': break;
		}
		fen++;
	}
	fen++;
	if (*fen != '-') {
		const int sq = (fen[0] - 'a') + 8 * (fen[1] - '1');
		pos->ep = bbSquare[sq];
	}
	while (*fen && *fen != ' ') fen++; fen++;
	pos->move50 = atoi(fen);
	if (flipped)
		FlipPosition(pos);
}

static char* ParseToken(char* string, char* token) {
	while (*string == ' ')
		string++;
	while (*string != ' ' && *string != '\0' && *string != '\n')
		*token++ = *string++;
	*token = '\0';
	return string;
}

static int MakeMove(Position* pos, const Move* move) {
	const int piece = PieceTypeOnSquare(pos, move->from);
	const int captured = PieceTypeOnSquare(pos, move->to);
	const U64 to = bbSquare[move->to];
	const U64 from = bbSquare[move->from];
	pos->move50++;
	if (captured != PT_NB || piece == PAWN)
		pos->move50 = 0;
	pos->color[0] ^= from | to;
	pos->pieces[piece] ^= from | to;
	if (piece == PAWN && to == pos->ep) {
		pos->color[1] ^= to >> 8;
		pos->pieces[PAWN] ^= to >> 8;
	}
	pos->ep = 0x0ULL;
	if (piece == PAWN && move->to - move->from == 16)
		pos->ep = to >> 8;
	if (captured != PT_NB) {
		pos->color[1] ^= to;
		pos->pieces[captured] ^= to;
	}
	if (piece == KING) {
		const U64 bb = move->to - move->from == 2 ? 0xa0ULL : move->to - move->from == -2 ? 0x9ULL : 0x0ULL;
		pos->color[0] ^= bb;
		pos->pieces[ROOK] ^= bb;
	}
	if (piece == PAWN && move->to >= 56) {
		pos->pieces[PAWN] ^= to;
		pos->pieces[move->promo] ^= to;
	}
	pos->castling[0] &= ((from | to) & 0x90ULL) == 0;
	pos->castling[1] &= ((from | to) & 0x11ULL) == 0;
	pos->castling[2] &= ((from | to) & 0x9000000000000000ULL) == 0;
	pos->castling[3] &= ((from | to) & 0x1100000000000000ULL) == 0;
	FlipPosition(pos);
	return !Attacked(pos, LSB(pos->color[1] & pos->pieces[KING]), 0);
}

static char* MoveToUci(Move move, int flip) {
	static char str[6] = { 0 };
	str[0] = 'a' + FileOf(move.from);
	str[1] = '1' + (flip ? (7 - RankOf(move.from)) : RankOf(move.from));
	str[2] = 'a' + FileOf(move.to);
	str[3] = '1' + (flip ? (7 - RankOf(move.to)) : RankOf(move.to));
	str[4] = "\0nbrq\0\0"[move.promo];
	return str;
}

static Move UciToMove(char* s, int flip) {
	Move m;
	m.from = (s[0] - 'a');
	int f = (s[1] - '1');
	m.from += 8 * (flip ? 7 - f : f);
	m.to = (s[2] - 'a');
	f = (s[3] - '1');
	m.to += 8 * (flip ? 7 - f : f);
	m.promo = PT_NB;
	switch (s[4]) {
	case 'N':
	case 'n':
		m.promo = KNIGHT;
		break;
	case 'B':
	case 'b':
		m.promo = BISHOP;
		break;
	case 'R':
	case 'r':
		m.promo = ROOK;
		break;
	case 'Q':
	case 'q':
		m.promo = QUEEN;
		break;
	}
	return m;
}

static void PrintBitboard(U64 bb) {
	const char* s = "   +---+---+---+---+---+---+---+---+\n";
	const char* t = "     A   B   C   D   E   F   G   H\n";
	printf(t);
	for (int r = 7; r >= 0; r--) {
		printf(s);
		printf(" %d |", r + 1);
		for (int f = 0; f < 8; f++) {
			int sq = r * 8 + f;
			printf(" %c |", bb & 1ull << sq ? 'x' : ' ');
		}
		printf(" %d \n", r + 1);
	}
	printf(s);
	printf(t);
}

static U64 Attacks(int pt, int sq, U64 blockers) {
	switch (pt) {
	case ROOK:
		return RookAttack(sq, blockers);
	case BISHOP:
		return BishopAttack(sq, blockers);
	case QUEEN:
		return RookAttack(sq, blockers) | BishopAttack(sq, blockers);
	case KNIGHT:
		return KnightAttack(sq);
	case KING:
		return KingAttack(sq);
	default:
		return 0;
	}
}

static int EvalPosition(Position* pos) {
	int phase = 0;
	int score = 0;
	int scoreMg = 0;
	int scoreEg = 0;
	int insufficient[2] = { 0 };
	U64 bbBlockers = pos->color[0] | pos->color[1];
	for (int c = WHITE; c < COLOR_NB; ++c) {
		U64 bbStart1 = pos->color[1] & pos->pieces[PAWN];
		U64 bbControl1 = SW(bbStart1) | SE(bbStart1);
		for (int pt = PAWN; pt < PT_NB; ++pt) {
			U64 copy = pos->color[0] & pos->pieces[pt];
			while (copy) {
				const int fr = (int)LSB(copy);
				copy &= copy - 1;
				scoreMg += mg_pst[pt][fr];
				scoreEg += eg_pst[pt][fr];
				phase += phaseVal[pt];
				insufficient[c] += insufVal[pt];
				if (pt > PAWN && pt < KING) {
					U64 bbAttack = Attacks(pt, fr, bbBlockers);
					score += Count(bbAttack & ~bbControl1);
				}
			}
		}
		U64 bbStart0 = pos->color[0] & pos->pieces[KING];
		U64 file0 = bbFiles[FileOf(LSB(bbStart0))];
		file0 |= East(file0) | West(file0);
		U64 bbAttack0 = file0 & (bbRanks[RANK_2] | bbRanks[RANK_3]) & ~(FILE_DBB | FILE_EBB);
		bbAttack0 &= (pos->color[0] & pos->pieces[PAWN]);
		scoreMg += Count(bbAttack0) * 10;
		scoreMg += Count(bbAttack0 & bbRanks[RANK_2]) * 10;
		FlipPosition(pos);
		score = -score;
		scoreMg = -scoreMg;
		scoreEg = -scoreEg;
	}
	if (max(insufficient[0], insufficient[1]) < 5)return 0;
	if (insufficient[score < 0] < 4)return 0;
	if (phase > 24) phase = 24;
	score += (scoreMg * phase + scoreEg * (24 - phase)) / 24;
	return (100 - pos->move50) * score / 100;
}

static int IsPseudolegalMove(const Position* pos, const Move move) {
	Move moves[256];
	const int num_moves = MoveGen(pos, moves, 0);
	for (int i = 0; i < num_moves; ++i)
		if (moves[i].from == move.from && moves[i].to == move.to)
			return 1;
	return 0;
}

static void PrintPv(const Position* pos, const Move move) {
	if (!IsPseudolegalMove(pos, move))
		return;
	const Position npos = *pos;
	if (!MakeMove(&npos, &move))
		return;
	printf(" %s", MoveToUci(move, pos->flipped));
	const U64 hash = GetHash(&npos);
	TTEntry* tt_entry = tt + (hash % tt_count);
	if (tt_entry->hash != hash || IsRepetition(&npos, hash))
		return;
	historyHash[historyCount++] = hash;
	PrintPv(&npos, tt_entry->move);
	historyCount--;
}

static int Permill() {
	int pm = 0;
	for (int n = 0; n < 1000; n++)
		if (tt[n].hash)
			pm++;
	return pm;
}

static void PrintInfo(Position* pos, int depth, int score) {
	printf("info depth %d score ", depth);
	if (abs(score) < MATE - MAX_PLY)
		printf("cp %d", score);
	else
		printf("mate %d", (score > 0 ? (MATE - score + 1) >> 1 : -(MATE + score) >> 1));
	printf(" time %lld", GetTimeMs() - info.timeStart);
	printf(" nodes %lld", info.nodes);
	printf(" hashfull %d pv", Permill());
	PrintPv(pos, ss[0].move);
	printf("\n");
}

static S16 SearchAlpha(Position* pos, int alpha, int beta, int depth, int ply, int doNull) {
	if (CheckUp(pos))
		return 0;
	int  mate_value = MATE - ply;
	if (alpha < -mate_value)
		alpha = -mate_value;
	if (beta > mate_value - 1)
		beta = mate_value - 1;
	if (alpha >= beta)
		return alpha;
	const int staticEval = EvalPosition(pos);
	if (ply >= MAX_PLY)
		return staticEval;
	const U64 inCheck = Attacked(pos, (int)LSB(pos->color[0] & pos->pieces[KING]), 1);
	if (inCheck)
		depth = max(1, depth + 1);
	int inQuiescence = depth < 1;
	const U64 hash = GetHash(pos);
	if (ply && !inQuiescence)
		if (pos->move50 >= 100 || IsRepetition(pos, hash))
			return 0;
	TTEntry* tt_entry = tt + (hash % tt_count);
	Move ttMove = { 0 };
	int inPv = beta - alpha > 1;
	if (tt_entry->hash == hash) {
		ttMove = tt_entry->move;
		if (!inPv && tt_entry->depth >= depth) {
			if (tt_entry->flag == EXACT)return tt_entry->score;
			if (tt_entry->flag == LOWER && tt_entry->score <= alpha)return tt_entry->score;
			if (tt_entry->flag == UPPER && tt_entry->score >= beta)return tt_entry->score;
		}
	}
	else
		depth -= depth > 3;
	if (inQuiescence && alpha < staticEval) {
		alpha = staticEval;
		if (alpha >= beta)
			return beta;
	}
	if (!inQuiescence && !inPv && !inCheck && ply && beta < MATE - MAX_PLY) {
		// REVERSE FUTILITY PRUNING
		if (depth < 8 && staticEval - 70 * depth >= beta)
			return staticEval - 70 * depth;
		// RAZORING
		if (depth <= 3 && staticEval + 300 + 60 * depth < alpha)
			depth--;
		// NULL MOVE PRUNING
		if (depth > 2 && staticEval >= beta && doNull && (pos->color[0] & (pos->pieces[KNIGHT] | pos->pieces[BISHOP] | pos->pieces[ROOK] | pos->pieces[QUEEN]))) {
			Position npos = *pos;
			FlipPosition(&npos);
			npos.ep = 0x0ULL;
			int R = depth >= 7 ? 4 : 3;
			int score = -SearchAlpha(&npos, -beta, -beta + 1, depth - R - 1, ply + 1, 0);
			if (score >= beta)
				return score;
		}
	}
	historyHash[historyCount++] = hash;
	U8 tt_flag = LOWER;
	S16 score;
	int legalMoves = 0;
	Move movesList[256];
	int quietMoves = 0;
	Move qList[256];
	const int movesCount = MoveGen(pos, movesList, inQuiescence);
	S64 scoreList[256];
	for (int j = 0; j < movesCount; ++j) {
		Move m = movesList[j];
		const int ptSou = PieceTypeOnSquare(pos, m.from);
		int ptDes = m.promo == PT_NB ? PieceTypeOnSquare(pos, m.to) : m.promo;
		if (Equal(m, ttMove))
			scoreList[j] = 1LL << 62;
		else if (ptDes != PT_NB)
			scoreList[j] = ((ptDes + 1) * (1LL << 54)) - ptSou;
		else if (Equal(m, ss[ply].killer1))
			scoreList[j] = 1LL << 50;
		else if (Equal(m, ss[ply].killer2))
			scoreList[j] = 1LL << 48;
		else
			scoreList[j] = hh[pos->flipped][m.from][m.to];
	}
	for (int i = 0; i < movesCount; ++i) {
		int bstIdx = i;
		for (int j = i + 1; j < movesCount; ++j)
			if (scoreList[bstIdx] < scoreList[j])
				bstIdx = j;
		Move move = movesList[bstIdx];
		scoreList[bstIdx] = scoreList[i];
		movesList[bstIdx] = movesList[i];

		// Delta pruning
		if (inQuiescence && !inCheck && staticEval + 50 + mx_material[PieceTypeOnSquare(pos, move.to)] < alpha)
			break;

		Position npos = *pos;
		if (!MakeMove(&npos, &move))
			continue;
		if (!legalMoves || depth < 4)
			score = -SearchAlpha(&npos, -beta, -alpha, depth - 1, ply + 1, 1);
		else {
			int r = !inPv;
			score = -SearchAlpha(&npos, -alpha - 1, -alpha, depth - 1 - r, ply + 1, 1);
			if (r && score > alpha)
				score = -SearchAlpha(&npos, -alpha - 1, -alpha, depth - 1, ply + 1, 1);
			if (score > alpha && score < beta)
				score = -SearchAlpha(&npos, -beta, -alpha, depth - 1, ply + 1, 1);
		}
		if (info.stop)
			break;
		legalMoves++;
		int isQuiet = move.promo == PT_NB && PieceTypeOnSquare(pos, move.to) == PT_NB && (PieceTypeOnSquare(pos, move.from) != PAWN || bbSquare[move.to] != pos->ep);
		if (isQuiet)
			qList[quietMoves++] = move;
		if (alpha < score) {
			alpha = score;
			tt_flag = EXACT;
			ss[ply].move = move;
			if (!ply && info.post)
				PrintInfo(pos, depth, score);
			if (alpha >= beta) {
				tt_flag = UPPER;
				if (isQuiet) {
					ss[ply].killer2 = ss[ply].killer1;
					ss[ply].killer1 = move;
				}
				int bonus = depth * depth;
				int h = hh[pos->flipped][move.from][move.to];
				h += bonus - h / 1024;
				hh[pos->flipped][move.from][move.to] = h;
				for (int i = 0; i < quietMoves; i++) {
					Move* m = &qList[i];
					int hm = hh[pos->flipped][m->from][m->to];
					hm -= bonus - hm / 1024;
					hh[pos->flipped][m->from][m->to] = hm;
				}
				break;
			}
		}
	}
	historyCount--;
	if (info.stop)
		return 0;
	if (!legalMoves && !inQuiescence)
		return inQuiescence ? alpha : inCheck ? ply - MATE : 0;
	tt_entry->hash = hash;
	tt_entry->move = ss[ply].move;
	tt_entry->depth = max(0, depth);
	tt_entry->score = alpha;
	tt_entry->flag = tt_flag;
	return alpha;
}

static void SearchIteratively(Position* pos) {
	HHClear();
	SSClear();
	TTClear();
	int score = 0;
	int alpha = -MATE;
	int beta = MATE;
	for (int depth = 1; depth <= info.depthLimit; ++depth) {
		int aspH = 16, aspL = 16;
		do {
			if (depth > 4) {
				alpha = score - aspL;
				beta = score + aspH;
			}
			score = SearchAlpha(pos, alpha, beta, depth, 0, 1);
			if (score <= alpha) {
				alpha -= aspL;
				aspL *= 2;
			}
			else if (score >= beta) {
				beta += aspH;
				aspH *= 2;
			}
			else
				break;
		} while (!info.stop);
		if (info.stop)
			break;
		if (info.timeLimit && GetTimeMs() - info.timeStart > info.timeLimit / 2)
			break;
	}
	if (info.post) {
		char* uci = MoveToUci(ss[0].move, pos->flipped);
		printf("bestmove %s\n", uci);
		fflush(stdout);
	}
}

static void ResetInfo() {
	info.timeStart = GetTimeMs();
	info.timeLimit = 0;
	info.depthLimit = MAX_PLY;
	info.nodesLimit = 0;
	info.nodes = 0;
	info.stop = FALSE;
	info.post = TRUE;
}

static inline void PerftDriver(Position* pos, int depth) {
	Move moves[256];
	const int num_moves = MoveGen(pos, moves, 0);
	for (int n = 0; n < num_moves; n++) {
		Position npos = *pos;
		if (!MakeMove(&npos, &moves[n]))
			continue;
		if (depth)
			PerftDriver(&npos, depth - 1);
		else
			info.nodes++;
	}
}

static int ShrinkNumber(U64 n) {
	if (n < 10000)
		return 0;
	if (n < 10000000)
		return 1;
	if (n < 10000000000)
		return 2;
	return 3;
}

static void PrintSummary(U64 time, U64 nodes) {
	U64 nps = (nodes * 1000) / max(time, 1);
	const char* units[] = { "", "k", "m", "g" };
	int sn = ShrinkNumber(nps);
	int p = pow(10, sn * 3);
	int b = pow(10, 3);
	printf("-----------------------------\n");
	printf("Time        : %llu\n", time);
	printf("Nodes       : %llu\n", nodes);
	printf("Nps         : %llu (%llu%s/s)\n", nps, nps / p, units[sn]);
	printf("-----------------------------\n");
}

static void PrintPerformanceHeader() {
	printf("-----------------------------\n");
	printf("ply      time        nodes\n");
	printf("-----------------------------\n");
}

//performance test
static void UciPerformance(Position* pos) {
	ResetInfo();
	PrintPerformanceHeader();
	info.depthLimit = 0;
	U64 elapsed = 0;
	while (elapsed < 3000) {
		PerftDriver(pos, info.depthLimit++);
		elapsed = GetTimeMs() - info.timeStart;
		printf(" %2d. %8llu %12llu\n", info.depthLimit, elapsed, info.nodes);
	}
	PrintSummary(elapsed, info.nodes);
}

//start benchmark
static void UciBench(Position* pos) {
	ResetInfo();
	PrintPerformanceHeader();
	info.depthLimit = 0;
	info.post = FALSE;
	U64 elapsed = 0;
	while (elapsed < 3000) {
		++info.depthLimit;
		SearchIteratively(pos);
		elapsed = GetTimeMs() - info.timeStart;
		printf(" %2d. %8llu %12llu\n", info.depthLimit, elapsed, info.nodes);
	}
	PrintSummary(elapsed, info.nodes);
}

static void ParsePosition(Position* pos, char* ptr) {
	char token[80], fen[80];
	ptr = ParseToken(ptr, token);
	if (strcmp(token, "fen") == 0) {
		fen[0] = '\0';
		while (1) {
			ptr = ParseToken(ptr, token);
			if (*token == '\0' || strcmp(token, "moves") == 0)
				break;
			strcat(fen, token);
			strcat(fen, " ");
		}
		SetFen(pos, fen);
	}
	else {
		ptr = ParseToken(ptr, token);
		SetFen(pos, START_FEN);
	}
	historyCount = 0;
	if (strcmp(token, "moves") == 0) {
		while (1) {
			ptr = ParseToken(ptr, token);
			if (*token == '\0')
				break;
			Move m = UciToMove(token, pos->flipped);
			if (PieceTypeOnSquare(pos, m.to) != PT_NB || PieceTypeOnSquare(pos, m.from) == PAWN)
				historyCount = 0;
			historyHash[historyCount++] = GetHash(pos);
			MakeMove(pos, &m);
		}
	}
}

static void ParseGo(Position* pos, char* command) {
	ResetInfo();
	int wtime = 0;
	int btime = 0;
	int winc = 0;
	int binc = 0;
	int movestogo = 32;
	char* argument = NULL;
	if (argument = strstr(command, "binc"))
		binc = atoi(argument + 5);
	if (argument = strstr(command, "winc"))
		winc = atoi(argument + 5);
	if (argument = strstr(command, "wtime"))
		wtime = max(1, atoi(argument + 6));
	if (argument = strstr(command, "btime"))
		btime = max(1, atoi(argument + 6));
	if ((argument = strstr(command, "movestogo")))
		movestogo = atoi(argument + 10);
	if ((argument = strstr(command, "movetime")))
		info.timeLimit = atoi(argument + 9);
	if ((argument = strstr(command, "depth")))
		info.depthLimit = atoi(argument + 6);
	if (argument = strstr(command, "nodes"))
		info.nodesLimit = atoi(argument + 5);
	int time = pos->flipped ? btime : wtime;
	int inc = pos->flipped ? binc : winc;
	if (time)
		info.timeLimit = max(1, min(time / movestogo + inc, time / 2));
	SearchIteratively(pos);
}

void UciCommand(Position* pos, char* line) {
	if (!strncmp(line, "ucinewgame", 10)) {}
	else if (!strncmp(line, "uci", 3)) {
		printf("id name %s\nuciok\n", NAME);
		fflush(stdout);
	}
	else if (!strncmp(line, "isready", 7)) {
		printf("readyok\n");
		fflush(stdout);
	}
	else if (!strncmp(line, "go", 2))ParseGo(pos, line + 2);
	else if (!strncmp(line, "position", 8))ParsePosition(pos, line + 8);
	else if (!strncmp(line, "print", 5))PrintBoard(pos);
	else if (!strncmp(line, "perft", 5))UciPerformance(pos);
	else if (!strncmp(line, "bench", 5))UciBench(pos);
	else if (!strncmp(line, "stop", 4))info.stop = TRUE;
	else if (!strncmp(line, "quit", 4))exit(0);
}

static void UciLoop(Position* pos) {
	char line[4000];
	while (fgets(line, sizeof(line), stdin))
		UciCommand(pos, line);
}

int main(const int argc, const char** argv) {
	Position pos;
	InitBitboards();
	InitHash();
	InitPst();
	printf("%s %s\n", NAME, VERSION);
	SetFen(&pos, START_FEN);
	UciLoop(&pos);
}
