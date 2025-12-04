BUILD ?= obj

CFLAGS += -g -O3 -mrtm -D_GNU_SOURCE=1
LDFLAGS += -no-pie

all: call_rdrand crosstalk
.PHONY: clean

call_rdrand-obj = $(addprefix ${BUILD}/, src/call_rdrand_core0.o)
crosstalk-obj = $(addprefix ${BUILD}/, src/leaker_core1.o)
fpvi-obj = $(addprefix ${BUILD}/, src/fpvi.o)

deps += $(call_rdrand-obj:.o=.d)
deps += $(crosstalk-obj:.o=.d)
deps += $(fpvi-obj:.o=.d)

-include ${deps}

call_rdrand: ${call_rdrand-obj}
	@echo "LD $(notdir $@)"
	@mkdir -p "$(dir $@)"
	@${CC} ${call_rdrand-obj} -o $@ ${LDFLAGS} ${CFLAGS}
	@rm -rf $(BUILD)

crosstalk: ${crosstalk-obj}
	@echo "LD $(notdir $@)"
	@mkdir -p "$(dir $@)"
	@${CC} ${crosstalk-obj} -o $@ ${LDFLAGS} ${CFLAGS}
	@rm -rf $(BUILD)

fpvi: ${fpvi-obj}
	@echo "LD $(notdir $@)"
	@mkdir -p "$(dir $@)"
	@${CC} ${fpvi-obj} -o $@ ${LDFLAGS} ${CFLAGS}
	@rm -rf $(BUILD)

$(BUILD)/%.o: %.c
	@echo "CC $<"
	@mkdir -p "$(dir $@)"
	@${CC} -c $< -o $@ ${CFLAGS} -MT $@ -MMD -MP -MF $(@:.o=.d)

clean:
	@rm -f call_rdrand crosstalk fpvi
